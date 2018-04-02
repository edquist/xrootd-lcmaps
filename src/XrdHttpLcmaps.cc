
#include <iostream>
#include <map>

#include <time.h>
#include <openssl/ssl.h>

#include <XrdHttp/XrdHttpSecXtractor.hh>
#include <XrdVersion.hh>
#include <XrdSys/XrdSysPthread.hh>
#include <XrdSec/XrdSecEntity.hh>

extern "C" {
#include "lcmaps.h"
}

#include "XrdLcmapsConfig.hh"
#include "XrdLcmapsKey.hh"

#define policy_count 1
static const char default_db  [] = "/etc/lcmaps.db";
static const char default_policy_name [] = "xrootd_policy";
static const char plugin_name [] = "XrdSecgsiAuthz";

// `proxy_app_verify_callback` comes from libvomsapi (but isn't found in the
// headers).  It extends OpenSSL's built-in certificate verify function with
// support for `old-style` proxies.
extern "C" {
  extern int proxy_app_verify_callback(X509_STORE_CTX *ctx, void *empty);
}

XrdVERSIONINFO(XrdHttpGetSecXtractor,"lcmaps");

// Someday we'll actually hook into the Xrootd logging system...
#define PRINT(y)    std::cerr << y << "\n";


inline uint64_t monotonic_time() {
  struct timespec tp;
#ifdef CLOCK_MONOTONIC_COARSE
  clock_gettime(CLOCK_MONOTONIC_COARSE, &tp);
#else
  clock_gettime(CLOCK_MONOTONIC, &tp);
#endif
  return tp.tv_sec + (tp.tv_nsec >= 500000000);
}


void
UpdateEntity(XrdSecEntity &out, XrdSecEntity const &in)
{
    if (in.name) { free(out.name); out.name = strdup(in.name); }
    if (in.host) { free(out.host); out.host = strdup(in.host); }
    if (in.vorg) { free(out.vorg); out.vorg = strdup(in.vorg); }
    if (in.role) { free(out.role); out.role = strdup(in.role); }
    if (in.grps) { free(out.grps); out.grps = strdup(in.grps); }
    if (in.creds && in.credslen > 0)
    {
        free(out.creds);
        out.creds = strdup(in.creds);
        out.credslen = in.credslen;
    }
    if (in.endorsements)
    {
        free(out.endorsements);
        out.endorsements = strdup(in.endorsements);
    }
    if (in.moninfo) { free(out.moninfo); out.moninfo = strdup(in.moninfo); }
}


void
FreeEntity(XrdSecEntity *&ent)
{
    free(ent->name);
    free(ent->host);
    free(ent->vorg);
    free(ent->role);
    free(ent->grps);
    free(ent->creds);
    free(ent->endorsements);
    free(ent->moninfo);
    delete ent;
}


class XrdMappingCache
{
public:


    ~XrdMappingCache()
    {
        for (KeyToEnt::iterator it = m_map.begin();
            it != m_map.end();
            it++)
        {
            FreeEntity(it->second.first);
        }
    }


    /**
     * Get the data associated with a particular key and store
     * it into the entity object.
     * If this returns false, then the key was not found.
     */
    bool get(std::string key, struct XrdSecEntity &entity)
    {
        time_t now = monotonic_time();
        m_mutex.Lock();
        if (now > m_last_clean + 100) {clean_tables();}
        KeyToEnt::const_iterator iter = m_map.find(key);
        m_mutex.UnLock();
        if (iter == m_map.end()) {
            return false;
        }
        UpdateEntity(entity, *iter->second.first);
        return true;
    }


    /**
     * Put the entity data into the map for a given key.
     * Makes a copy of the caller's data if the key was not already present
     * This function is thread safe.
     *
     * If result is false, then the key was already in the map.
     */
    void try_put(std::string key, struct XrdSecEntity const &entity)
    {
        time_t now = monotonic_time();
        m_mutex.Lock();
        XrdSecEntity *new_ent = new XrdSecEntity();
        std::pair<KeyToEnt::iterator, bool> ret = m_map.insert(std::make_pair(key, std::make_pair(new_ent, now)));
        if (ret.second)
        {
            ValueType &value = ret.first->second;
            XrdSecEntity *ent = value.first;
            UpdateEntity(*ent, entity);
        }
        else
        {
            delete new_ent;
        }
        m_mutex.UnLock();
    }


    static XrdMappingCache &GetInstance()
    {
        return m_cache;
    }

private:

    // No copying...
    XrdMappingCache& operator=(XrdMappingCache const&);
    XrdMappingCache(XrdMappingCache const&);

    XrdMappingCache()
      : m_last_clean(monotonic_time())
    {
    }

    /**
     * MUST CALL LOCKED
     */
    void clean_tables()
    {
        m_last_clean = monotonic_time();
        time_t expiry = m_last_clean + 100;
        KeyToEnt::iterator it = m_map.begin();
        while (it != m_map.end()) {
            if (it->second.second < expiry) {
                FreeEntity(it->second.first);
                m_map.erase(it++);
            } else {
                ++it;
            }
        }
    }

    typedef std::pair<XrdSecEntity*, time_t> ValueType;
    typedef std::map<std::string, ValueType> KeyToEnt;

    XrdSysMutex m_mutex;
    time_t m_last_clean;
    KeyToEnt m_map;
    static XrdMappingCache m_cache;
};

XrdMappingCache XrdMappingCache::m_cache;


class XrdHttpLcmaps : public XrdHttpSecXtractor
{
public:

    virtual ~XrdHttpLcmaps() {}

    virtual int GetSecData(XrdLink *, XrdSecEntity &entity, SSL *ssl)
    {
        static const char err_pfx[] = "ERROR in AuthzFun: ";
        static const char inf_pfx[] = "INFO in AuthzFun: ";

        //PRINT(inf_pfx << "Running security information extractor");

        // Per OpenSSL docs, the ref count of peer_chain is not incremented.
        // Hence, we do not free this later.
        STACK_OF(X509) * peer_chain = SSL_get_peer_cert_chain(ssl);

        // The refcount here is incremented.
        X509 * peer_certificate = SSL_get_peer_certificate(ssl);

        // No remote client?  Add nothing to the entity, but do not
        // fail.
        if (!peer_certificate)
        {
            return 0;
        }
        // This one is a more difficult call.  We should have disabled session reuse.
        if (!peer_chain)
        {
            PRINT(inf_pfx << "No available peer certificate chain.");
            X509_free(peer_certificate);
            if (SSL_session_reused(ssl))
            {
                PRINT(inf_pfx << "SSL session was unexpectedly reused.");
                return -1;
            }
            return 0;
        }

        STACK_OF(X509) * full_stack = sk_X509_new_null();
        sk_X509_push(full_stack, peer_certificate);
        for (int idx = 0; idx < sk_X509_num(peer_chain); idx++) {
            sk_X509_push(full_stack, sk_X509_value(peer_chain, idx)); 
        }

        std::string key = GetKey(peer_certificate, peer_chain, entity);
        XrdMappingCache &mcache = XrdMappingCache::GetInstance();
        PRINT(inf_pfx << "Lookup with key " << key);
        if (mcache.get(key, entity)) {
            PRINT(inf_pfx << "Using cached entity with username " << entity.name);
            sk_X509_free(full_stack);
            X509_free(peer_certificate);
            return 0;
        }

        // Grab the global mutex - lcmaps is not thread-safe.
        // TODO(bbockelm): Cache lookups
        XrdSysMutexHelper lock(&m_mutex);

        char  *poolindex = NULL;
        uid_t  uid = -1;
        gid_t *pgid_list = NULL, *sgid_list = NULL;
        int    npgid = 0, nsgid = 0;
        lcmaps_request_t request = NULL; // Typically, the RSL

        // To manage const cast issues
        char * policy_name_copy = strdup(default_policy_name);

        int rc = lcmaps_run_with_stack_of_x509_and_return_account(
            full_stack,
            -1, // mapcounter
            request,
            policy_count,
            &policy_name_copy,
            &uid,
            &pgid_list,
            &npgid,
            &sgid_list,
            &nsgid,
            &poolindex);

        if (policy_name_copy) {
            free(policy_name_copy);
        }

        sk_X509_free(full_stack);
        X509_free(peer_certificate);

        if (pgid_list) {free(pgid_list);}
        if (sgid_list) {free(sgid_list);}
        if (poolindex) {free(poolindex);}

        // If there's a client cert but LCMAPS fails, we _do_ want to
        // fail the whole thing.
        if (rc) {
            PRINT(err_pfx << "LCMAPS failed or denied mapping");
            return -1;
        }

        PRINT(inf_pfx << "Got uid " << uid);
        struct passwd * pw = getpwuid(uid);
        if (pw == NULL) {
            return -1;
        }

        free(entity.moninfo);
        entity.moninfo = entity.name;
        entity.name = strdup(pw->pw_name);

        mcache.try_put(key, entity);

        return 0;
    }

    virtual int Init(SSL_CTX *sslctx, int)
    {
        // TODO(bbockelm): OpenSSL docs note that peer_chain is not available
        // in reused sessions.  We should build a session cache, but we just
        // disable sessions for now.
        SSL_CTX_set_session_cache_mode(sslctx, SSL_SESS_CACHE_OFF);
        SSL_CTX_set_options(sslctx, SSL_OP_NO_TICKET);

        // Utilize VOMS's peer certificate verification function (which
        // supports old-style proxies).
        SSL_CTX_set_cert_verify_callback(sslctx, proxy_app_verify_callback, 0);
        return 0;
    }

    XrdHttpLcmaps(XrdSysError *)
    {
    }

    int Config(const char *cfg)
    {
        return XrdSecgsiAuthzConfig(cfg);
    }

private:

    XrdSysError *eDest;
    static XrdSysMutex m_mutex;

};


XrdSysMutex XrdHttpLcmaps::m_mutex;


extern "C" XrdHttpSecXtractor *XrdHttpGetSecXtractor(XrdHttpSecXtractorArgs)
{
    XrdHttpLcmaps *extractor = new XrdHttpLcmaps(eDest);
    if (extractor->Config(parms)) {
        delete extractor;
        return NULL;
    }
    return extractor;
}


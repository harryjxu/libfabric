/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Amazon.com, Inc. or its affiliates. All rights reserved. */

#include <assert.h>
#include <ofi_util.h>
#include <infiniband/efadv.h>

#include "efa.h"
#include "efa_av.h"
#include "efa_domain.h"
#include "uthash.h"

/**
 * @brief initialize the address handle map in the domain
 */
int efa_domain_ah_init(struct efa_domain *domain)
{
    int ret;
    
    domain->ah_map = NULL;
    ret = ofi_genlock_init(&domain->ah_map_lock, OFI_LOCK_MUTEX);
    if (ret) {
        EFA_WARN(FI_LOG_DOMAIN, "Failed to initialize ah_map_lock: %s\n", fi_strerror(-ret));
        return ret;
    }
    
    return 0;
}

/**
 * @brief cleanup the address handle map in the domain
 */
int efa_domain_ah_cleanup(struct efa_domain *domain)
{
    struct efa_ah *ah, *tmp;
    int err = 0;
    
    ofi_genlock_lock(&domain->ah_map_lock);
    
    /* Clean up any remaining address handles */
    HASH_ITER(hh, domain->ah_map, ah, tmp) {
        HASH_DEL(domain->ah_map, ah);
        if (ah->refcnt > 0) {
            EFA_WARN(FI_LOG_DOMAIN, "Address handle still has %d references during cleanup\n", ah->refcnt);
        }
        err = ibv_destroy_ah(ah->ibv_ah);
        if (err) {
            EFA_WARN(FI_LOG_DOMAIN, "ibv_destroy_ah failed! err=%d\n", err);
        }
        free(ah);
    }
    
    domain->ah_map = NULL;
    ofi_genlock_unlock(&domain->ah_map_lock);
    ofi_genlock_destroy(&domain->ah_map_lock);
    
    return 0;
}

/**
 * @brief allocate an ibv_ah object from GID using domain's shared map
 */
struct efa_ah *efa_domain_ah_alloc(struct efa_domain *domain, const uint8_t *gid)
{
    struct ibv_pd *ibv_pd = domain->ibv_pd;
    struct efa_ah *efa_ah;
    struct ibv_ah_attr ibv_ah_attr = { 0 };
    struct efadv_ah_attr efa_ah_attr = { 0 };
    int err;
    
    ofi_genlock_lock(&domain->ah_map_lock);
    
    /* Check if this address handle already exists in the domain */
    efa_ah = NULL;
    HASH_FIND(hh, domain->ah_map, gid, EFA_GID_LEN, efa_ah);
    if (efa_ah) {
        efa_ah->refcnt += 1;
        ofi_genlock_unlock(&domain->ah_map_lock);
        return efa_ah;
    }
    
    /* Create a new address handle */
    efa_ah = malloc(sizeof(struct efa_ah));
    if (!efa_ah) {
        errno = FI_ENOMEM;
        EFA_WARN(FI_LOG_DOMAIN, "cannot allocate memory for efa_ah\n");
        ofi_genlock_unlock(&domain->ah_map_lock);
        return NULL;
    }
    
    ibv_ah_attr.port_num = 1;
    ibv_ah_attr.is_global = 1;
    memcpy(ibv_ah_attr.grh.dgid.raw, gid, EFA_GID_LEN);
    efa_ah->ibv_ah = ibv_create_ah(ibv_pd, &ibv_ah_attr);
    if (!efa_ah->ibv_ah) {
        EFA_WARN(FI_LOG_DOMAIN, "ibv_create_ah failed! errno: %d\n", errno);
        goto err_free_efa_ah;
    }
    
    err = efadv_query_ah(efa_ah->ibv_ah, &efa_ah_attr, sizeof(efa_ah_attr));
    if (err) {
        errno = err;
        EFA_WARN(FI_LOG_DOMAIN, "efadv_query_ah failed! err: %d\n", err);
        goto err_destroy_ibv_ah;
    }
    
    efa_ah->refcnt = 1;
    efa_ah->ahn = efa_ah_attr.ahn;
    memcpy(efa_ah->gid, gid, EFA_GID_LEN);
    HASH_ADD(hh, domain->ah_map, gid, EFA_GID_LEN, efa_ah);
    
    ofi_genlock_unlock(&domain->ah_map_lock);
    return efa_ah;
    
err_destroy_ibv_ah:
    ibv_destroy_ah(efa_ah->ibv_ah);
err_free_efa_ah:
    free(efa_ah);
    ofi_genlock_unlock(&domain->ah_map_lock);
    return NULL;
}

/**
 * @brief release an efa_ah object from the domain's map
 */
void efa_domain_ah_release(struct efa_domain *domain, struct efa_ah *ah)
{
    int err;
    
    ofi_genlock_lock(&domain->ah_map_lock);
    
#if ENABLE_DEBUG
    struct efa_ah *tmp;
    HASH_FIND(hh, domain->ah_map, ah->gid, EFA_GID_LEN, tmp);
    assert(tmp == ah);
#endif
    
    assert(ah->refcnt > 0);
    ah->refcnt -= 1;
    if (ah->refcnt == 0) {
        HASH_DEL(domain->ah_map, ah);
        err = ibv_destroy_ah(ah->ibv_ah);
        if (err)
            EFA_WARN(FI_LOG_DOMAIN, "ibv_destroy_ah failed! err=%d\n", err);
        free(ah);
    }
    
    ofi_genlock_unlock(&domain->ah_map_lock);
}
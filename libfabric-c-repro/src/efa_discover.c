/*
 * efa_discover.c - Discover and list available EFA devices
 * 
 * This program uses fi_getinfo to enumerate all available EFA devices
 * and print their properties. This is the first step to verify our
 * build system and libfabric integration.
 */

#include "common.h"

static void print_efa_device(struct fi_info *info, int index) {
    char addr_str[256];
    size_t len;
    
    printf("\n=== EFA Device %d ===\n", index);
    printf("Provider: %s\n", info->fabric_attr->prov_name);
    printf("Fabric name: %s\n", info->fabric_attr->name);
    printf("Domain name: %s\n", info->domain_attr->name);
    
    /* Print addressing info */
    if (info->src_addr) {
        len = sizeof(addr_str);
        fi_av_straddr(NULL, info->src_addr, addr_str, &len);
        printf("Source address: %s\n", addr_str);
        printf("Source address format: %d\n", info->addr_format);
    }
    
    /* Print endpoint attributes */
    printf("EP type: ");
    switch (info->ep_attr->type) {
        case FI_EP_RDM: printf("RDM (Reliable Datagram)\n"); break;
        case FI_EP_DGRAM: printf("DGRAM (Datagram)\n"); break;
        case FI_EP_MSG: printf("MSG (Message)\n"); break;
        default: printf("Unknown (%d)\n", info->ep_attr->type); break;
    }
    
    printf("Protocol: ");
    switch (info->ep_attr->protocol) {
        case FI_PROTO_EFA: printf("EFA\n"); break;
        case FI_PROTO_RXD: printf("RXD\n"); break;
        /* FI_PROTO_RXR might not be defined in older versions */
        default: printf("0x%x\n", info->ep_attr->protocol); break;
    }
    
    /* Print capabilities */
    printf("Capabilities: 0x%lx (", info->caps);
    if (info->caps & FI_MSG) printf("MSG ");
    if (info->caps & FI_RMA) printf("RMA ");
    if (info->caps & FI_TAGGED) printf("TAGGED ");
    if (info->caps & FI_ATOMIC) printf("ATOMIC ");
    if (info->caps & FI_MULTI_RECV) printf("MULTI_RECV ");
    if (info->caps & FI_SOURCE) printf("SOURCE ");
    if (info->caps & FI_READ) printf("READ ");
    if (info->caps & FI_WRITE) printf("WRITE ");
    if (info->caps & FI_SEND) printf("SEND ");
    if (info->caps & FI_RECV) printf("RECV ");
    if (info->caps & FI_REMOTE_READ) printf("REMOTE_READ ");
    if (info->caps & FI_REMOTE_WRITE) printf("REMOTE_WRITE ");
    printf(")\n");
    
    /* Print mode requirements */
    printf("Mode flags: 0x%lx", info->mode);
    if (info->mode) {
        printf(" (");
        if (info->mode & FI_CONTEXT) printf("CONTEXT ");
        /* FI_LOCAL_MR is deprecated, skip it */
        if (info->mode & FI_MSG_PREFIX) printf("MSG_PREFIX ");
        if (info->mode & FI_ASYNC_IOV) printf("ASYNC_IOV ");
        if (info->mode & FI_RX_CQ_DATA) printf("RX_CQ_DATA ");
        printf(")");
    }
    printf("\n");
    
    /* Print memory registration mode */
    printf("MR mode: 0x%x", info->domain_attr->mr_mode);
    if (info->domain_attr->mr_mode) {
        printf(" (");
        if (info->domain_attr->mr_mode & FI_MR_LOCAL) printf("LOCAL ");
        if (info->domain_attr->mr_mode & FI_MR_RAW) printf("RAW ");
        if (info->domain_attr->mr_mode & FI_MR_VIRT_ADDR) printf("VIRT_ADDR ");
        if (info->domain_attr->mr_mode & FI_MR_ALLOCATED) printf("ALLOCATED ");
        if (info->domain_attr->mr_mode & FI_MR_PROV_KEY) printf("PROV_KEY ");
        if (info->domain_attr->mr_mode & FI_MR_MMU_NOTIFY) printf("MMU_NOTIFY ");
        if (info->domain_attr->mr_mode & FI_MR_RMA_EVENT) printf("RMA_EVENT ");
        if (info->domain_attr->mr_mode & FI_MR_ENDPOINT) printf("ENDPOINT ");
        printf(")");
    }
    printf("\n");
    
    /* Print sizes */
    printf("Max message size: %zu\n", info->ep_attr->max_msg_size);
    /* tagged_size might not exist in this version, just use max_msg_size */
    printf("Max tagged size: %zu\n", info->ep_attr->max_msg_size);
    printf("Inject size: %zu\n", info->tx_attr->inject_size);
    printf("IOV limit: %zu\n", info->tx_attr->iov_limit);
}

int main(void) {
    struct fi_info *hints = NULL;
    struct fi_info *info = NULL;
    struct fi_info *cur;
    int ret;
    int device_count = 0;
    
    LOG_INFO("EFA Device Discovery Tool");
    LOG_INFO("libfabric version: %d.%d", 
             FI_MAJOR(FABRIC_VERSION), FI_MINOR(FABRIC_VERSION));
    
    /* Setup hints to search for EFA providers */
    hints = fi_allocinfo();
    if (!hints) {
        LOG_ERROR("Failed to allocate fi_info");
        return EXIT_FAILURE;
    }
    
    /* Request EFA provider specifically */
    hints->fabric_attr->prov_name = strdup(EFA_PROVIDER);
    
    /* We want RDM endpoints for our test */
    hints->ep_attr->type = FI_EP_RDM;
    
    /* Request capabilities we'll use */
    hints->caps = FI_MSG | FI_TAGGED | FI_SEND | FI_RECV;
    
    /* Allow provider to return all matching interfaces */
    hints->mode = 0;
    
    LOG_INFO("Searching for EFA devices...");
    
    /* Get all matching fabric interfaces */
    ret = fi_getinfo(FABRIC_VERSION, NULL, NULL, 0, hints, &info);
    if (ret) {
        if (ret == -FI_ENODATA) {
            LOG_ERROR("No EFA devices found. Make sure:");
            LOG_ERROR("  1. You are running on an instance with EFA support");
            LOG_ERROR("  2. EFA drivers are installed");
            LOG_ERROR("  3. You are in the neuron environment (use-neuron)");
        } else {
            LOG_ERROR("fi_getinfo failed: %s (%d)", fi_strerror(-ret), ret);
        }
        fi_freeinfo(hints);
        return EXIT_FAILURE;
    }
    
    /* Count and display devices */
    for (cur = info; cur; cur = cur->next) {
        device_count++;
    }
    
    LOG_INFO("Found %d EFA device(s)", device_count);
    
    /* Print details for each device */
    device_count = 0;
    for (cur = info; cur; cur = cur->next) {
        print_efa_device(cur, device_count++);
    }
    
    /* Try to also list with less restrictive hints */
    printf("\n=== Additional provider information ===\n");
    
    struct fi_info *all_info = NULL;
    struct fi_info *all_hints = fi_allocinfo();
    if (all_hints) {
        /* Don't restrict provider */
        ret = fi_getinfo(FABRIC_VERSION, NULL, NULL, 0, all_hints, &all_info);
        if (ret == 0) {
            int other_count = 0;
            for (cur = all_info; cur; cur = cur->next) {
                if (strcmp(cur->fabric_attr->prov_name, EFA_PROVIDER) != 0) {
                    if (other_count == 0) {
                        printf("\nOther providers available:\n");
                    }
                    printf("  - %s (%s)\n", 
                           cur->fabric_attr->prov_name,
                           cur->domain_attr->name);
                    other_count++;
                }
            }
            if (other_count == 0) {
                printf("No other providers found (only EFA available)\n");
            }
            fi_freeinfo(all_info);
        }
        fi_freeinfo(all_hints);
    }
    
    /* Cleanup */
    fi_freeinfo(hints);
    fi_freeinfo(info);
    
    LOG_INFO("\nDiscovery complete!");
    return EXIT_SUCCESS;
}
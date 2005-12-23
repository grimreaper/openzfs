/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 * Data-Link Services Module
 */

#include	<sys/types.h>
#include	<sys/stream.h>
#include	<sys/strsun.h>
#include	<sys/sysmacros.h>
#include	<sys/atomic.h>
#include	<sys/dlpi.h>
#include	<sys/vlan.h>
#include	<sys/ethernet.h>
#include	<sys/byteorder.h>
#include	<sys/mac.h>

#include	<sys/dls.h>
#include	<sys/dls_impl.h>
#include	<sys/dls_soft_ring.h>

static kmem_cache_t	*i_dls_impl_cachep;
static uint32_t		i_dls_impl_count;

static kstat_t	*dls_ksp = (kstat_t *)NULL;
struct dls_kstats dls_kstat =
{
	{ "soft_ring_pkt_drop", KSTAT_DATA_UINT32 },
};


/*
 * Private functions.
 */

/*ARGSUSED*/
static int
i_dls_constructor(void *buf, void *arg, int kmflag)
{
	dls_impl_t	*dip = buf;

	bzero(buf, sizeof (dls_impl_t));

	rw_init(&(dip->di_lock), NULL, RW_DRIVER, NULL);
	return (0);
}

/*ARGSUSED*/
static void
i_dls_destructor(void *buf, void *arg)
{
	dls_impl_t	*dip = buf;

	ASSERT(dip->di_dvp == NULL);
	ASSERT(dip->di_mnh == NULL);
	ASSERT(dip->di_dmap == NULL);
	ASSERT(!dip->di_bound);
	ASSERT(dip->di_rx == NULL);
	ASSERT(dip->di_txinfo == NULL);

	rw_destroy(&(dip->di_lock));
}

static void
i_dls_notify(void *arg, mac_notify_type_t type)
{
	dls_impl_t		*dip = arg;

	switch (type) {
	case MAC_NOTE_UNICST:
		mac_unicst_get(dip->di_mh, dip->di_unicst_addr);
		break;

	case MAC_NOTE_PROMISC:
		/*
		 * Every time the MAC interface changes promiscuity we
		 * need to reset our transmit information.
		 */
		dip->di_txinfo = mac_tx_get(dip->di_mh);
		break;
	}
}

static mblk_t *
i_dls_ether_header(dls_impl_t *dip, const uint8_t *daddr, uint16_t sap,
    uint_t pri)
{
	struct ether_header		*ehp;
	struct ether_vlan_header	*evhp;
	const mac_info_t		*mip;
	uint_t				addr_length;
	uint16_t			vid;
	mblk_t				*mp;

	mip = dip->di_mip;
	addr_length = mip->mi_addr_length;

	/*
	 * Check whether the DLSAP value is legal for ethernet.
	 */
	if (!SAP_LEGAL(mip->mi_media, sap))
		return (NULL);

	/*
	 * If the interface is a VLAN interface then we need VLAN packet
	 * headers.
	 */
	if ((vid = dip->di_dvp->dv_id) != VLAN_ID_NONE)
		goto vlan;

	/*
	 * Allocate a normal ethernet packet header.
	 */
	if ((mp = allocb(sizeof (struct ether_header), BPRI_HI)) == NULL)
		return (NULL);

	/*
	 * Copy in the given address as the destination, our current unicast
	 * address as the source and the given sap as the type/length.
	 */
	ehp = (struct ether_header *)mp->b_rptr;
	bcopy(daddr, &(ehp->ether_dhost), addr_length);
	bcopy(dip->di_unicst_addr, &(ehp->ether_shost), addr_length);
	ehp->ether_type = htons(sap);

	mp->b_wptr += sizeof (struct ether_header);
	return (mp);

vlan:
	/*
	 * Allocate a VLAN ethernet packet header.
	 */
	if ((mp = allocb(sizeof (struct ether_vlan_header), BPRI_HI)) == NULL)
		return (NULL);

	/*
	 * Copy in the given address as the destination, our current unicast
	 * address as the source, the VLAN tpid and tci and the given sap as
	 * the type/length.
	 */
	evhp = (struct ether_vlan_header *)mp->b_rptr;
	bcopy(daddr, &(evhp->ether_dhost), addr_length);
	bcopy(dip->di_unicst_addr, &(evhp->ether_shost), addr_length);
	evhp->ether_tpid = htons(VLAN_TPID);
	evhp->ether_tci = htons(VLAN_TCI(pri, ETHER_CFI, vid));
	evhp->ether_type = htons(sap);

	mp->b_wptr += sizeof (struct ether_vlan_header);
	return (mp);
}

/*ARGSUSED*/
static void
i_dls_ether_header_info(dls_impl_t *dip, mblk_t *mp, dls_header_info_t *dhip)
{
	struct ether_header		*ehp;
	struct ether_vlan_header	*evhp;
	uint16_t			type_length;
	uint16_t			tci;

	ASSERT(MBLKL(mp) >= sizeof (struct ether_header));
	ehp = (struct ether_header *)mp->b_rptr;

	/*
	 * Determine whether to parse a normal or VLAN ethernet header.
	 */
	if ((type_length = ntohs(ehp->ether_type)) == VLAN_TPID)
		goto vlan;

	/*
	 * Specify the length of the header.
	 */
	dhip->dhi_length = sizeof (struct ether_header);

	/*
	 * Get the destination address.
	 */
	dhip->dhi_daddr = (const uint8_t *)&(ehp->ether_dhost);

	/*
	 * If the destination address was a group address then
	 * dl_group_address field should be non-zero.
	 */
	dhip->dhi_isgroup = (dhip->dhi_daddr[0] & 0x01);

	/*
	 * Get the source address.
	 */
	dhip->dhi_saddr = (uint8_t *)&(ehp->ether_shost);

	/*
	 * Get the ethertype
	 */
	dhip->dhi_ethertype = (type_length > ETHERMTU) ? type_length : 0;

	/*
	 * The VLAN identifier must be VLAN_ID_NONE.
	 */
	dhip->dhi_vid = VLAN_ID_NONE;

	return;

vlan:
	ASSERT(MBLKL(mp) >= sizeof (struct ether_vlan_header));
	evhp = (struct ether_vlan_header *)mp->b_rptr;

	/*
	 * Specify the length of the header.
	 */
	dhip->dhi_length = sizeof (struct ether_vlan_header);

	/*
	 * Get the destination address.
	 */
	dhip->dhi_daddr = (const uint8_t *)&(evhp->ether_dhost);

	/*
	 * If the destination address was a group address then
	 * dl_group_address field should be non-zero.
	 */
	dhip->dhi_isgroup = (dhip->dhi_daddr[0] & 0x01);

	/*
	 * Get the source address.
	 */
	dhip->dhi_saddr = (uint8_t *)&(evhp->ether_shost);

	/*
	 * Get the ethertype
	 */
	type_length = ntohs(evhp->ether_type);
	dhip->dhi_ethertype = (type_length > ETHERMTU) ? type_length : 0;
	ASSERT(dhip->dhi_ethertype != VLAN_TPID);

	/*
	 * Get the VLAN identifier.
	 */
	tci = ntohs(evhp->ether_tci);
	dhip->dhi_vid = VLAN_ID(tci);
}

static void
dls_stat_init()
{
	if ((dls_ksp = kstat_create("dls", 0, "dls_stat",
	    "net", KSTAT_TYPE_NAMED,
	    sizeof (dls_kstat) / sizeof (kstat_named_t),
	    KSTAT_FLAG_VIRTUAL)) == NULL) {
		cmn_err(CE_WARN,
		"DLS: failed to create kstat structure for dls stats");
		return;
	}
	dls_ksp->ks_data = (void *)&dls_kstat;
	kstat_install(dls_ksp);
}

static void
dls_stat_destroy()
{
	kstat_delete(dls_ksp);
}

/*
 * Module initialization functions.
 */

void
dls_init(void)
{
	/*
	 * Create a kmem_cache of dls_impl_t.
	 */
	i_dls_impl_cachep = kmem_cache_create("dls_cache",
	    sizeof (dls_impl_t), 0, i_dls_constructor, i_dls_destructor, NULL,
	    NULL, NULL, 0);
	ASSERT(i_dls_impl_cachep != NULL);
	soft_ring_init();
	dls_stat_init();
}

int
dls_fini(void)
{
	/*
	 * If there are any dls_impl_t in use then return EBUSY.
	 */
	if (i_dls_impl_count != 0)
		return (EBUSY);

	/*
	 * Destroy the kmem_cache.
	 */
	kmem_cache_destroy(i_dls_impl_cachep);
	dls_stat_destroy();
	return (0);
}

/*
 * Client function.
 */

int
dls_create(const char *name, const char *dev, uint_t port)
{
	return (dls_vlan_create(name, dev, port, 0));
}

int
dls_destroy(const char *name)
{
	return (dls_vlan_destroy(name));
}

int
dls_open(const char *name, dls_channel_t *dcp)
{
	dls_impl_t	*dip;
	dls_vlan_t	*dvp;
	dls_link_t	*dlp;
	int		err;

	/*
	 * Get a reference to the named dls_vlan_t.
	 * Tagged vlans get created automatically.
	 */
	if ((err = dls_vlan_hold(name, &dvp, B_TRUE)) != 0)
		return (err);

	/*
	 * Allocate a new dls_impl_t.
	 */
	dip = kmem_cache_alloc(i_dls_impl_cachep, KM_SLEEP);
	dip->di_dvp = dvp;

	/*
	 * Cache a copy of the MAC interface handle, a pointer to the
	 * immutable MAC info and a copy of the current MAC address.
	 */
	dlp = dvp->dv_dlp;
	dip->di_mh = dlp->dl_mh;
	dip->di_mip = dlp->dl_mip;

	mac_unicst_get(dip->di_mh, dip->di_unicst_addr);

	/*
	 * Set the MAC transmit information.
	 */
	dip->di_txinfo = mac_tx_get(dip->di_mh);

	/*
	 * Set up packet header constructor and parser functions. (We currently
	 * only support ethernet).
	 */
	ASSERT(dip->di_mip->mi_media == DL_ETHER);
	dip->di_header = i_dls_ether_header;
	dip->di_header_info = i_dls_ether_header_info;

	/*
	 * Add a notification function so that we get updates from the MAC.
	 */
	dip->di_mnh = mac_notify_add(dip->di_mh, i_dls_notify, (void *)dip);

	/*
	 * Bump the kmem_cache count to make sure it is not prematurely
	 * destroyed.
	 */
	atomic_add_32(&i_dls_impl_count, 1);

	/*
	 * Hand back a reference to the dls_impl_t.
	 */
	*dcp = (dls_channel_t)dip;
	return (0);
}

void
dls_close(dls_channel_t dc)
{
	dls_impl_t		*dip = (dls_impl_t *)dc;
	dls_vlan_t		*dvp;
	dls_link_t		*dlp;
	dls_multicst_addr_t	*p;
	dls_multicst_addr_t	*nextp;

	dls_active_clear(dc);

	rw_enter(&(dip->di_lock), RW_WRITER);

	/*
	 * Remove the notify function.
	 */
	mac_notify_remove(dip->di_mh, dip->di_mnh);
	dip->di_mnh = NULL;

	/*
	 * If the dls_impl_t is bound then unbind it.
	 */
	dvp = dip->di_dvp;
	dlp = dvp->dv_dlp;

	if (dip->di_bound) {
		rw_exit(&(dip->di_lock));
		dls_link_remove(dlp, dip);
		rw_enter(&(dip->di_lock), RW_WRITER);
		dip->di_rx = NULL;
		dip->di_rx_arg = NULL;
		dip->di_bound = B_FALSE;
	}

	/*
	 * Walk the list of multicast addresses, disabling each at the MAC.
	 */
	for (p = dip->di_dmap; p != NULL; p = nextp) {
		(void) mac_multicst_remove(dip->di_mh, p->dma_addr);
		nextp = p->dma_nextp;
		kmem_free(p, sizeof (dls_multicst_addr_t));
	}
	dip->di_dmap = NULL;

	rw_exit(&(dip->di_lock));

	/*
	 * If the MAC has been set in promiscuous mode then disable it.
	 */
	(void) dls_promisc(dc, 0);

	/*
	 * Free the dls_impl_t back to the cache.
	 */
	dip->di_dvp = NULL;
	dip->di_txinfo = NULL;

	if (dip->di_soft_ring_list != NULL) {
		soft_ring_set_destroy(dip->di_soft_ring_list,
		    dip->di_soft_ring_size);
		dip->di_soft_ring_list = NULL;
	}
	dip->di_soft_ring_size = 0;

	kmem_cache_free(i_dls_impl_cachep, dip);

	/*
	 * Decrement the reference count to allow the cache to be destroyed
	 * if there are no more dls_impl_t.
	 */
	atomic_add_32(&i_dls_impl_count, -1);

	/*
	 * Release our reference to the dls_vlan_t allowing that to be
	 * destroyed if there are no more dls_impl_t. An unreferenced tagged
	 * vlan gets destroyed automatically.
	 */
	dls_vlan_rele(dvp);
}

mac_handle_t
dls_mac(dls_channel_t dc)
{
	dls_impl_t	*dip = (dls_impl_t *)dc;

	return (dip->di_mh);
}

uint16_t
dls_vid(dls_channel_t dc)
{
	dls_impl_t	*dip = (dls_impl_t *)dc;

	return (dip->di_dvp->dv_id);
}

int
dls_bind(dls_channel_t dc, uint16_t sap)
{
	dls_impl_t	*dip = (dls_impl_t *)dc;
	dls_link_t	*dlp;

	/*
	 * Check to see the value is legal for the media type.
	 */
	if (!SAP_LEGAL(dip->di_mip->mi_media, sap))
		return (EINVAL);

	/*
	 * Set up the dls_impl_t to mark it as able to receive packets.
	 */
	rw_enter(&(dip->di_lock), RW_WRITER);
	ASSERT(!dip->di_bound);
	dip->di_sap = sap;
	dip->di_bound = B_TRUE;
	rw_exit(&(dip->di_lock));

	/*
	 * Now bind the dls_impl_t by adding it into the hash table in the
	 * dls_link_t.
	 *
	 * NOTE: This must be done without the dls_impl_t lock being held
	 *	 otherwise deadlock may ensue.
	 */
	dlp = dip->di_dvp->dv_dlp;
	dls_link_add(dlp,
	    (dip->di_promisc & DLS_PROMISC_SAP) ? DLS_SAP_PROMISC :
	    (uint32_t)sap, dip);

	return (0);
}

void
dls_unbind(dls_channel_t dc)
{
	dls_impl_t	*dip = (dls_impl_t *)dc;
	dls_link_t	*dlp;

	/*
	 * Unbind the dls_impl_t by removing it from the hash table in the
	 * dls_link_t.
	 *
	 * NOTE: This must be done without the dls_impl_t lock being held
	 *	 otherise deadlock may enuse.
	 */
	dlp = dip->di_dvp->dv_dlp;
	dls_link_remove(dlp, dip);

	/*
	 * Mark the dls_impl_t as unable to receive packets This will make
	 * sure that 'receives in flight' will not come our way.
	 */
	dip->di_bound = B_FALSE;
}

int
dls_promisc(dls_channel_t dc, uint32_t flags)
{
	dls_impl_t	*dip = (dls_impl_t *)dc;
	dls_link_t	*dlp;
	int		err = 0;

	ASSERT(!(flags & ~(DLS_PROMISC_SAP | DLS_PROMISC_MULTI |
	    DLS_PROMISC_PHYS)));

	/*
	 * Check if we need to turn on 'all sap' mode.
	 */
	rw_enter(&(dip->di_lock), RW_WRITER);
	dlp = dip->di_dvp->dv_dlp;
	if ((flags & DLS_PROMISC_SAP) &&
	    !(dip->di_promisc & DLS_PROMISC_SAP)) {
		dip->di_promisc |= DLS_PROMISC_SAP;
		if (!dip->di_bound)
			goto multi;

		rw_exit(&(dip->di_lock));
		dls_link_remove(dlp, dip);
		dls_link_add(dlp, DLS_SAP_PROMISC, dip);
		rw_enter(&(dip->di_lock), RW_WRITER);
		goto multi;
	}

	/*
	 * Check if we need to turn off 'all sap' mode.
	 */
	if (!(flags & DLS_PROMISC_SAP) &&
	    (dip->di_promisc & DLS_PROMISC_SAP)) {
		dip->di_promisc &= ~DLS_PROMISC_SAP;
		if (!dip->di_bound)
			goto multi;

		rw_exit(&(dip->di_lock));
		dls_link_remove(dlp, dip);
		dls_link_add(dlp, dip->di_sap, dip);
		rw_enter(&(dip->di_lock), RW_WRITER);
	}

multi:
	/*
	 * It's easiest to add the txloop handler up-front; if promiscuous
	 * mode cannot be enabled, then we'll remove it before returning.
	 * Use dl_promisc_lock to prevent racing with another thread also
	 * manipulating the promiscuous state on another dls_impl_t associated
	 * with the same dls_link_t.
	 */
	mutex_enter(&dlp->dl_promisc_lock);
	if (dlp->dl_npromisc == 0 &&
	    (flags & (DLS_PROMISC_MULTI|DLS_PROMISC_PHYS))) {
		ASSERT(dlp->dl_mth == NULL);
		dlp->dl_mth = mac_txloop_add(dlp->dl_mh, dlp->dl_loopback, dlp);
	}

	/*
	 * Turn on or off 'all multicast' mode, if necessary.
	 */
	if (flags & DLS_PROMISC_MULTI) {
		if (!(dip->di_promisc & DLS_PROMISC_MULTI)) {
			err = mac_promisc_set(dip->di_mh, B_TRUE, MAC_PROMISC);
			if (err != 0)
				goto done;
			dip->di_promisc |= DLS_PROMISC_MULTI;
			dlp->dl_npromisc++;
		}
	} else {
		if (dip->di_promisc & DLS_PROMISC_MULTI) {
			err = mac_promisc_set(dip->di_mh, B_FALSE, MAC_PROMISC);
			if (err != 0)
				goto done;
			dip->di_promisc &= ~DLS_PROMISC_MULTI;
			dlp->dl_npromisc--;
		}
	}

	/*
	 * Turn on or off 'all physical' mode, if necessary.
	 */
	if (flags & DLS_PROMISC_PHYS) {
		if (!(dip->di_promisc & DLS_PROMISC_PHYS)) {
			err = mac_promisc_set(dip->di_mh, B_TRUE, MAC_PROMISC);
			if (err != 0)
				goto done;
			dip->di_promisc |= DLS_PROMISC_PHYS;
			dlp->dl_npromisc++;
		}
	} else {
		if (dip->di_promisc & DLS_PROMISC_PHYS) {
			err = mac_promisc_set(dip->di_mh, B_FALSE, MAC_PROMISC);
			if (err != 0)
				goto done;
			dip->di_promisc &= ~DLS_PROMISC_PHYS;
			dlp->dl_npromisc--;
		}
	}

done:
	if (dlp->dl_npromisc == 0 && dlp->dl_mth != NULL) {
		mac_txloop_remove(dlp->dl_mh, dlp->dl_mth);
		dlp->dl_mth = NULL;
	}

	ASSERT(dlp->dl_npromisc == 0 || dlp->dl_mth != NULL);
	mutex_exit(&dlp->dl_promisc_lock);

	rw_exit(&(dip->di_lock));
	return (err);
}

int
dls_multicst_add(dls_channel_t dc, const uint8_t *addr)
{
	dls_impl_t		*dip = (dls_impl_t *)dc;
	int			err;
	dls_multicst_addr_t	**pp;
	dls_multicst_addr_t	*p;
	uint_t			addr_length;

	/*
	 * Check whether the address is in the list of enabled addresses for
	 * this dls_impl_t.
	 */
	rw_enter(&(dip->di_lock), RW_WRITER);
	addr_length = dip->di_mip->mi_addr_length;
	for (pp = &(dip->di_dmap); (p = *pp) != NULL; pp = &(p->dma_nextp)) {
		if (bcmp(addr, p->dma_addr, addr_length) == 0) {
			/*
			 * It is there so there's nothing to do.
			 */
			err = 0;
			goto done;
		}
	}

	/*
	 * Allocate a new list item.
	 */
	if ((p = kmem_zalloc(sizeof (dls_multicst_addr_t),
	    KM_NOSLEEP)) == NULL) {
		err = ENOMEM;
		goto done;
	}

	/*
	 * Enable the address at the MAC.
	 */
	if ((err = mac_multicst_add(dip->di_mh, addr)) != 0) {
		kmem_free(p, sizeof (dls_multicst_addr_t));
		goto done;
	}

	/*
	 * The address is now enabled at the MAC so add it to the list.
	 */
	bcopy(addr, p->dma_addr, addr_length);
	*pp = p;

done:
	rw_exit(&(dip->di_lock));
	return (err);
}

int
dls_multicst_remove(dls_channel_t dc, const uint8_t *addr)
{
	dls_impl_t		*dip = (dls_impl_t *)dc;
	int			err;
	dls_multicst_addr_t	**pp;
	dls_multicst_addr_t	*p;
	uint_t			addr_length;

	/*
	 * Find the address in the list of enabled addresses for this
	 * dls_impl_t.
	 */
	rw_enter(&(dip->di_lock), RW_WRITER);
	addr_length = dip->di_mip->mi_addr_length;
	for (pp = &(dip->di_dmap); (p = *pp) != NULL; pp = &(p->dma_nextp)) {
		if (bcmp(addr, p->dma_addr, addr_length) == 0)
			break;
	}

	/*
	 * If we walked to the end of the list then the given address is
	 * not currently enabled for this dls_impl_t.
	 */
	if (p == NULL) {
		err = ENOENT;
		goto done;
	}

	/*
	 * Disable the address at the MAC.
	 */
	if ((err = mac_multicst_remove(dip->di_mh, addr)) != 0)
		goto done;

	/*
	 * Remove the address from the list.
	 */
	*pp = p->dma_nextp;
	kmem_free(p, sizeof (dls_multicst_addr_t));

done:
	rw_exit(&(dip->di_lock));
	return (err);
}

mblk_t *
dls_header(dls_channel_t dc, const uint8_t *addr, uint16_t sap, uint_t pri)
{
	dls_impl_t	*dip = (dls_impl_t *)dc;

	return (dip->di_header(dip, addr, sap, pri));
}

void
dls_header_info(dls_channel_t dc, mblk_t *mp, dls_header_info_t *dhip)
{
	dls_impl_t	*dip = (dls_impl_t *)dc;

	dip->di_header_info(dip, mp, dhip);
}

void
dls_rx_set(dls_channel_t dc, dls_rx_t rx, void *arg)
{
	dls_impl_t	*dip = (dls_impl_t *)dc;

	rw_enter(&(dip->di_lock), RW_WRITER);
	dip->di_rx = rx;
	dip->di_rx_arg = arg;
	rw_exit(&(dip->di_lock));
}

mblk_t *
dls_tx(dls_channel_t dc, mblk_t *mp)
{
	const mac_txinfo_t *mtp = ((dls_impl_t *)dc)->di_txinfo;

	return (mtp->mt_fn(mtp->mt_arg, mp));
}

/*
 * Exported functions.
 */

#define	ADDR_MATCH(_addr_a, _addr_b, _length, _match)			\
	{								\
		uint_t	i;						\
									\
		/*							\
		 * Make sure the addresses are 16 bit aligned and that	\
		 * the length is an even number of octets.		\
		 */							\
		ASSERT(IS_P2ALIGNED((_addr_a), sizeof (uint16_t)));	\
		ASSERT(IS_P2ALIGNED((_addr_b), sizeof (uint16_t)));	\
		ASSERT((_length & 1) == 0);				\
									\
		(_match) = B_TRUE;					\
		for (i = 0; i < (_length) >> 1; i++) {			\
			if (((uint16_t *)(_addr_a))[i] !=		\
			    ((uint16_t *)(_addr_b))[i]) {		\
				(_match) = B_FALSE;			\
				break;					\
			}						\
		}							\
	}

boolean_t
dls_accept(dls_impl_t *dip, const uint8_t *daddr, dls_rx_t *di_rx,
    void **di_rx_arg)
{
	boolean_t		match;
	dls_multicst_addr_t	*dmap;
	uint_t			addr_length = dip->di_mip->mi_addr_length;

	/*
	 * We must not accept packets if the dls_impl_t is not marked as bound
	 * or is being removed.
	 */
	rw_enter(&(dip->di_lock), RW_READER);
	if (!dip->di_bound || dip->di_removing)
		goto refuse;

	/*
	 * If the dls_impl_t is in 'all physical' mode then always accept.
	 */
	if (dip->di_promisc & DLS_PROMISC_PHYS)
		goto accept;

	/*
	 * Check to see if the destination address matches the dls_impl_t
	 * unicast address.
	 */
	ADDR_MATCH(daddr, dip->di_unicst_addr, addr_length, match);
	if (match)
		goto accept;

	/*
	 * Check for a 'group' address. If it is not then refuse it since we
	 * already know it does not match the unicast address.
	 */
	if (!(daddr[0] & 0x01))
		goto refuse;

	/*
	 * If the address is broadcast then the dls_impl_t will always accept
	 * it.
	 */
	ADDR_MATCH(daddr, dip->di_mip->mi_brdcst_addr, addr_length,
	    match);
	if (match)
		goto accept;

	/*
	 * If a group address is not broadcast then it must be multicast so
	 * check it against the list of addresses enabled for this dls_impl_t
	 * or accept it unconditionally if the dls_impl_t is in 'all
	 * multicast' mode.
	 */
	if (dip->di_promisc & DLS_PROMISC_MULTI)
		goto accept;

	for (dmap = dip->di_dmap; dmap != NULL; dmap = dmap->dma_nextp) {
		ADDR_MATCH(daddr, dmap->dma_addr, addr_length, match);
		if (match)
			goto accept;
	}

refuse:
	rw_exit(&(dip->di_lock));
	return (B_FALSE);

accept:
	/*
	 * Since we hold di_lock here, the returned di_rx and di_rx_arg will
	 * always be in sync.
	 */
	*di_rx = dip->di_rx;
	*di_rx_arg = dip->di_rx_arg;
	rw_exit(&(dip->di_lock));
	return (B_TRUE);
}

/*ARGSUSED*/
boolean_t
dls_accept_loopback(dls_impl_t *dip, const uint8_t *daddr, dls_rx_t *di_rx,
    void **di_rx_arg)
{
	/*
	 * We must not accept packets if the dls_impl_t is not marked as bound
	 * or is being removed.
	 */
	rw_enter(&(dip->di_lock), RW_READER);
	if (!dip->di_bound || dip->di_removing)
		goto refuse;

	/*
	 * A dls_impl_t should only accept loopback packets if it is in
	 * 'all physical' mode.
	 */
	if (dip->di_promisc & DLS_PROMISC_PHYS)
		goto accept;

refuse:
	rw_exit(&(dip->di_lock));
	return (B_FALSE);

accept:
	/*
	 * Since we hold di_lock here, the returned di_rx and di_rx_arg will
	 * always be in sync.
	 */
	*di_rx = dip->di_rx;
	*di_rx_arg = dip->di_rx_arg;
	rw_exit(&(dip->di_lock));
	return (B_TRUE);
}

boolean_t
dls_active_set(dls_channel_t dc)
{
	dls_impl_t	*dip = (dls_impl_t *)dc;
	dls_link_t	*dlp = dip->di_dvp->dv_dlp;

	rw_enter(&dip->di_lock, RW_WRITER);

	/* If we're already active, then there's nothing more to do. */
	if (dip->di_active) {
		rw_exit(&dip->di_lock);
		return (B_TRUE);
	}

	/*
	 * If this is the first active client on this link, notify
	 * the mac that we're becoming an active client.
	 */
	if (dlp->dl_nactive == 0 && !mac_active_set(dlp->dl_mh)) {
		rw_exit(&dip->di_lock);
		return (B_FALSE);
	}
	dip->di_active = B_TRUE;
	mutex_enter(&dlp->dl_lock);
	dlp->dl_nactive++;
	mutex_exit(&dlp->dl_lock);
	rw_exit(&dip->di_lock);
	return (B_TRUE);
}

void
dls_active_clear(dls_channel_t dc)
{
	dls_impl_t	*dip = (dls_impl_t *)dc;
	dls_link_t	*dlp = dip->di_dvp->dv_dlp;

	rw_enter(&dip->di_lock, RW_WRITER);

	if (!dip->di_active)
		goto out;
	dip->di_active = B_FALSE;

	mutex_enter(&dlp->dl_lock);
	if (--dlp->dl_nactive == 0)
		mac_active_clear(dip->di_mh);
	mutex_exit(&dlp->dl_lock);
out:
	rw_exit(&dip->di_lock);
}

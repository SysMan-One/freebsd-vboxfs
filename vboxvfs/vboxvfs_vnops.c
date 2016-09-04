/*
 * Copyright (C) 2008-2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/namei.h>
#include <sys/kernel.h>
#include <sys/types.h>
#include <sys/malloc.h>
#include <sys/stat.h>
#include <sys/bio.h>
#include <sys/conf.h>
#include <sys/buf.h>
#include <sys/iconv.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/dirent.h>
#include <sys/queue.h>
#include <sys/unistd.h>
#include <sys/endian.h>

#include <vm/uma.h>

#include "vboxvfs.h"

/*
 * Prototypes for VBOXVFS vnode operations
 */
static vop_create_t	vboxfs_create;
static vop_mknod_t	vboxfs_mknod;
static vop_open_t	vboxfs_open;
static vop_close_t	vboxfs_close;
static vop_access_t	vboxfs_access;
static vop_getattr_t	vboxfs_getattr;
static vop_setattr_t	vboxfs_setattr;
static vop_read_t	vboxfs_read;
static vop_readlink_t	vboxfs_readlink;
static vop_write_t	vboxfs_write;
static vop_fsync_t	vboxfs_fsync;
static vop_remove_t	vboxfs_remove;
static vop_link_t	vboxfs_link;
static vop_cachedlookup_t	vboxfs_lookup;
static vop_rename_t	vboxfs_rename;
static vop_mkdir_t	vboxfs_mkdir;
static vop_rmdir_t	vboxfs_rmdir;
static vop_symlink_t	vboxfs_symlink;
static vop_readdir_t	vboxfs_readdir;
static vop_print_t	vboxfs_print;
static vop_pathconf_t	vboxfs_pathconf;
static vop_advlock_t	vboxfs_advlock;
static vop_getextattr_t	vboxfs_getextattr;
static vop_ioctl_t	vboxfs_ioctl;
static vop_inactive_t	vboxfs_inactive;
static vop_reclaim_t	vboxfs_reclaim;
static vop_vptofh_t	vboxfs_vptofh;

struct vop_vector vboxfs_vnodeops = {
	.vop_default	= &default_vnodeops,

	.vop_access	= vboxfs_access,
	.vop_advlock	= vboxfs_advlock,
	.vop_close	= vboxfs_close,
	.vop_create	= vboxfs_create,
	.vop_fsync	= vboxfs_fsync,
	.vop_getattr	= vboxfs_getattr,
	.vop_getextattr = vboxfs_getextattr,
	.vop_inactive	= vboxfs_inactive,
	.vop_ioctl	= vboxfs_ioctl,
	.vop_link	= vboxfs_link,
	.vop_lookup	= vfs_cache_lookup,
	.vop_cachedlookup	= vboxfs_lookup,
	.vop_mkdir	= vboxfs_mkdir,
	.vop_mknod	= vboxfs_mknod,
	.vop_open	= vboxfs_open,
	.vop_pathconf	= vboxfs_pathconf,
	.vop_print	= vboxfs_print,
	.vop_read	= vboxfs_read,
	.vop_readdir	= vboxfs_readdir,
	.vop_readlink	= vboxfs_readlink,
	.vop_reclaim	= vboxfs_reclaim,
	.vop_remove	= vboxfs_remove,
	.vop_rename	= vboxfs_rename,
	.vop_rmdir	= vboxfs_rmdir,
	.vop_setattr	= vboxfs_setattr,
	.vop_vptofh 	= vboxfs_vptofh,
	.vop_symlink	= vboxfs_symlink,
	.vop_write	= vboxfs_write,
	.vop_bmap	= VOP_EOPNOTSUPP
};

static uint64_t
vsfnode_cur_time_usec(void)
{
        struct timeval now;

        getmicrotime(&now);

	return (now.tv_sec*1000 + now.tv_usec);
}

static int
vsfnode_stat_cached(struct vboxfs_node *np)
{
	return (vsfnode_cur_time_usec() - np->sf_stat_time) <
	    np->vboxfsmp->sf_stat_ttl * 1000UL;
}

static int
vsfnode_update_stat_cache(struct vboxfs_node *np)
{
	int error;

	error = sfprov_get_attr(np->vboxfsmp->sf_handle, np->sf_path,
	    &np->sf_stat);
#if 0
	if (error == ENOENT)
		sfnode_make_stale(node);
#endif
	if (error == 0)
		np->sf_stat_time = vsfnode_cur_time_usec();

	return (error);
}

/*
 * Need to clear v_object for insmntque failure.
 */
static void
vboxfs_insmntque_dtr(struct vnode *vp, void *dtr_arg)
{

	// XXX: vboxfs_destroy_vobject(vp, vp->v_object);
	vp->v_object = NULL;
	vp->v_data = NULL;
	vp->v_op = &dead_vnodeops;
	vgone(vp);
	vput(vp);
}

/*
 * Allocates a new vnode for the node node or returns a new reference to
 * an existing one if the node had already a vnode referencing it.  The
 * resulting locked vnode is returned in *vpp.
 *
 * Returns zero on success or an appropriate error code on failure.
 */
int
vboxfs_alloc_vp(struct mount *mp, struct vboxfs_node *node, int lkflag,
    struct vnode **vpp)
{
	struct vnode *vp;
	int error;

	error = 0;
loop:
	VBOXFS_NODE_LOCK(node);
loop1:
	if ((vp = node->sf_vnode) != NULL) {
		MPASS((node->sf_vpstate & VBOXFS_VNODE_DOOMED) == 0);
		VI_LOCK(vp);
		if ((node->sf_type == VDIR && node->sf_parent == NULL) ||
		    ((vp->v_iflag & VI_DOOMED) != 0 &&
		    (lkflag & LK_NOWAIT) != 0)) {
			VI_UNLOCK(vp);
			VBOXFS_NODE_UNLOCK(node);
			error = ENOENT;
			vp = NULL;
			goto out;
		}
		if ((vp->v_iflag & VI_DOOMED) != 0) {
			VI_UNLOCK(vp);
			node->sf_vpstate |= VBOXFS_VNODE_WRECLAIM;
			while ((node->sf_vpstate & VBOXFS_VNODE_WRECLAIM) != 0) {
				msleep(&node->sf_vnode, VBOXFS_NODE_MTX(node),
				    0, "vsfE", 0);
			}
			goto loop1;
		}
		VBOXFS_NODE_UNLOCK(node);
		error = vget(vp, lkflag | LK_INTERLOCK, curthread);
		if (error == ENOENT)
			goto loop;
		if (error != 0) {
			vp = NULL;
			goto out;
		}

		/*
		 * Make sure the vnode is still there after
		 * getting the interlock to avoid racing a free.
		 */
		if (node->sf_vnode == NULL || node->sf_vnode != vp) {
			vput(vp);
			goto loop;
		}

		goto out;
	}

	if ((node->sf_vpstate & VBOXFS_VNODE_DOOMED) ||
	    (node->sf_type == VDIR && node->sf_parent == NULL)) {
		VBOXFS_NODE_UNLOCK(node);
		error = ENOENT;
		vp = NULL;
		goto out;
	}

	/*
	 * otherwise lock the vp list while we call getnewvnode
	 * since that can block.
	 */
	if (node->sf_vpstate & VBOXFS_VNODE_ALLOCATING) {
		node->sf_vpstate |= VBOXFS_VNODE_WANT;
		error = msleep((caddr_t) &node->sf_vpstate,
		    VBOXFS_NODE_MTX(node), PDROP | PCATCH,
		    "vboxfs_alloc_vp", 0);
		if (error)
			return error;

		goto loop;
	} else
		node->sf_vpstate |= VBOXFS_VNODE_ALLOCATING;
	
	VBOXFS_NODE_UNLOCK(node);

	/* Get a new vnode and associate it with our node. */
	error = getnewvnode("vboxfs", mp, &vboxfs_vnodeops, &vp);
	if (error != 0)
		goto unlock;
	MPASS(vp != NULL);

	/* lkflag is ignored, the lock is exclusive */
	(void) vn_lock(vp, lkflag | LK_RETRY);

	vp->v_data = node;
	vp->v_type = node->sf_type;

	/* Type-specific initialization. */
	switch (node->sf_type) {
	case VBLK:
		/* FALLTHROUGH */
	case VCHR:
		/* FALLTHROUGH */
	case VLNK:
		/* FALLTHROUGH */
	case VSOCK:
		/* FALLTHROUGH */
	case VFIFO:
		break;
	case VREG:
#if 0 
		vm_object_t object;
		object = node->sf_reg.sf_aobj;
		VM_OBJECT_WLOCK(object);
		VI_LOCK(vp);
		KASSERT(vp->v_object == NULL, ("Not NULL v_object in vsf"));
		vp->v_object = object;
		object->un_pager.swp.swp_vsf = vp;
		vm_object_set_flag(object, OBJ_VBOXFS);
		VI_UNLOCK(vp);
		VM_OBJECT_WUNLOCK(object);
#endif
		break;
	case VDIR:
		MPASS(node->sf_parent != NULL);
		if (node->sf_parent == node)
			vp->v_vflag |= VV_ROOT;
		break;

	default:
		panic("vboxfs_alloc_vp: type %p %d", node, (int)node->sf_type);
	}
	if (vp->v_type != VFIFO)
		VN_LOCK_ASHARE(vp);

	error = insmntque1(vp, mp, vboxfs_insmntque_dtr, NULL);
	if (error)
		vp = NULL;

unlock:
	VBOXFS_NODE_LOCK(node);

	MPASS(node->sf_vpstate & VBOXFS_VNODE_ALLOCATING);
	node->sf_vpstate &= ~VBOXFS_VNODE_ALLOCATING;
	node->sf_vnode = vp;

	if (node->sf_vpstate & VBOXFS_VNODE_WANT) {
		node->sf_vpstate &= ~VBOXFS_VNODE_WANT;
		VBOXFS_NODE_UNLOCK(node);
		wakeup((caddr_t) &node->sf_vpstate);
	} else
		VBOXFS_NODE_UNLOCK(node);

out:
	*vpp = vp;

#ifdef INVARIANTS
	if (error == 0) {
		MPASS(*vpp != NULL && VOP_ISLOCKED(*vpp));
		VBOXFS_NODE_LOCK(node);
		MPASS(*vpp == node->sf_vnode);
		VBOXFS_NODE_UNLOCK(node);
	}
#endif

	return error;
}

/*
 * Destroys the association between the vnode vp and the node it
 * references.
 */
void
vboxfs_free_vp(struct vnode *vp)
{
	struct vboxfs_node *node;

	node = VP_TO_VBOXFS_NODE(vp);

	VBOXFS_NODE_ASSERT_LOCKED(node);
	node->sf_vnode = NULL;
	if ((node->sf_vpstate & VBOXFS_VNODE_WRECLAIM) != 0)
		wakeup(&node->sf_vnode);
	node->sf_vpstate &= ~VBOXFS_VNODE_WRECLAIM;
	vp->v_data = NULL;
}

static int
vboxfs_vn_get_ino_alloc(struct mount *mp, void *arg, int lkflags,
    struct vnode **rvp)
{

	return (vboxfs_alloc_vp(mp, arg, lkflags, rvp));
}

/*
 * Construct a new pathname given an sfnode plus an optional tail component.
 * This handles ".." and "."
 */
static char *
sfnode_construct_path(struct vboxfs_node *node, char *tail)
{
	char *p;

	if (strcmp(tail, ".") == 0 || strcmp(tail, "..") == 0)
		panic("construct path for %s", tail);
	p = malloc(strlen(node->sf_path) + 1 + strlen(tail) + 1, M_VBOXVFS, M_WAITOK);
	strcpy(p, node->sf_path);
	strcat(p, "/");
	strcat(p, tail);
	return (p);
}

static int
vboxfs_access(struct vop_access_args *ap)
{
	struct vnode *vp = ap->a_vp;
	accmode_t accmode = ap->a_accmode;

	if ((accmode & VWRITE) && (vp->v_mount->mnt_flag & MNT_RDONLY)) {
		switch (vp->v_type) {
		case VDIR:
		case VLNK:
		case VREG:
			return (EROFS);
			/* NOT REACHED */
		default:
			break;
		}
	}
	return (vaccess(vp->v_type, 0444, 0, 0,
	    accmode, ap->a_cred, NULL));
}

/*
 * Clears the (cached) directory listing for the node.
 */
static void
vfsnode_clear_dir_list(struct vboxfs_node *np)
{
	while (np->sf_dir_list != NULL) {
		sffs_dirents_t *next = np->sf_dir_list->sf_next;
		free(np->sf_dir_list, M_VBOXVFS);
		np->sf_dir_list = next;
	}
}

static int
vboxfs_open(struct vop_open_args *ap)
{
	struct vboxfs_node *np;
	sfp_file_t *fp;
	int error;

	np = VP_TO_VBOXFS_NODE(ap->a_vp);
	/*
	 * XXX need to populate sf_path somehow.  This information is not
	 *     provided to VOP_OPEN().  This must be why the Solaris
	 *     version has 'sfnode's in it.
	 */
	error = sfprov_open(np->vboxfsmp->sf_handle, np->sf_path, &fp);
	if (error != 0)
		return (error);

	np->sf_file = fp;
	vnode_create_vobject(ap->a_vp, 0, ap->a_td);

	return (0);
}

static void
vfsnode_invalidate_stat_cache(struct vboxfs_node *np)
{
	np->sf_stat_time = 0;
}

static int
vboxfs_close(struct vop_close_args *ap)
{
	
	struct vnode *vp = ap->a_vp;
	struct vboxfs_node *np;

	np = VP_TO_VBOXFS_NODE(vp);

	/*
	 * Free the directory entries for the node. We do this on this call
	 * here because the directory node may not become inactive for a long
	 * time after the readdir is over. Case in point, if somebody cd's into
	 * the directory then it won't become inactive until they cd away again.
	 * In such a case we would end up with the directory listing not getting
	 * updated (i.e. the result of 'ls' always being the same) until they
	 * change the working directory.
	 */
	vfsnode_clear_dir_list(np);

	vfsnode_invalidate_stat_cache(np);

	if (np->sf_file != NULL) {
		(void) sfprov_close(np->sf_file);
		np->sf_file = NULL;
	}

	return (0);
}

static int
vboxfs_getattr(struct vop_getattr_args *ap)
{
	struct vnode 		*vp = ap->a_vp;
	struct vattr 		*vap = ap->a_vap;
	struct vboxfs_node	*np = VP_TO_VBOXFS_NODE(vp);
	struct vboxfs_mnt  	*mp = np->vboxfsmp;
	mode_t			mode;
	int			error = 0;

	mode = 0;
	vap->va_type = vp->v_type;
	
	vap->va_nlink = 1;		/* number of references to file */
	vap->va_uid = mp->sf_uid;	/* owner user id */
	vap->va_gid = mp->sf_gid;	/* owner group id */
	vap->va_rdev = NODEV;		/* device the special file represents */ 
	vap->va_gen = VNOVAL;		/* generation number of file */
	vap->va_flags = 0;		/* flags defined for file */
	vap->va_filerev = 0;		/* file modification number */
	vap->va_vaflags = 0;		/* operations flags */
	vap->va_fileid = np->sf_ino;	/* file id */
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
	if (vap->va_fileid == 0)
		vap->va_fileid = 2;

	vap->va_atime.tv_sec = VNOVAL;
	vap->va_atime.tv_nsec = VNOVAL;
	vap->va_mtime.tv_sec = VNOVAL;
	vap->va_mtime.tv_nsec = VNOVAL;
	vap->va_ctime.tv_sec = VNOVAL;
	vap->va_ctime.tv_nsec = VNOVAL;

	if (!vsfnode_stat_cached(np)) {
		error = vsfnode_update_stat_cache(np);
		if (error != 0)
			goto done;
	}

	vap->va_atime = np->sf_stat.sf_atime;
	vap->va_mtime = np->sf_stat.sf_mtime;
	vap->va_ctime = np->sf_stat.sf_ctime;

	mode = np->sf_stat.sf_mode;

	vap->va_mode = mode;	/* TODO: mask files access mode and type */
	if (S_ISDIR(mode)) {
		vap->va_type = VDIR;	/* vnode type (for create) */
		vap->va_mode = mp->sf_dmode != 0 ? (mp->sf_dmode & 0777) : vap->va_mode;
		vap->va_mode &= ~mp->sf_dmask;
		vap->va_mode |= S_IFDIR;
	} else if (S_ISREG(mode)) {
		vap->va_type = VREG;
		vap->va_mode = mp->sf_fmode != 0 ? (mp->sf_fmode & 0777) : vap->va_mode;
		vap->va_mode &= ~mp->sf_fmask;
		vap->va_mode |= S_IFREG;
	} else if (S_ISFIFO(mode))
		vap->va_type = VFIFO;
	else if (S_ISCHR(mode))
		vap->va_type = VCHR;
	else if (S_ISBLK(mode))
		vap->va_type = VBLK;
	else if (S_ISLNK(mode)) {
		vap->va_type = VLNK;
		vap->va_mode = mp->sf_fmode != 0 ? (mp->sf_fmode & 0777) : vap->va_mode;
		vap->va_mode &= ~mp->sf_fmask;
		vap->va_mode |= S_IFLNK;
	} else if (S_ISSOCK(mode))
		vap->va_type = VSOCK;

	vap->va_size = np->sf_stat.sf_size;
	vap->va_blocksize = 512;
	/* bytes of disk space held by file */
   	vap->va_bytes = (np->sf_stat.sf_alloc + 511) / 512;

done:
	return (error);
}

static int
vboxfs_setattr(struct vop_setattr_args *ap)
{
	
	struct vnode 		*vp = ap->a_vp;
	struct vattr 		*vap = ap->a_vap;
	struct vboxfs_node	*np = VP_TO_VBOXFS_NODE(vp);
	int			error;
	mode_t			mode;

	mode = vap->va_mode;
	if (vp->v_type == VREG)
		mode |= S_IFREG;
	else if (vp->v_type == VDIR)
		mode |= S_IFDIR;
	else if (vp->v_type == VBLK)
		mode |= S_IFBLK;
	else if (vp->v_type == VCHR)
		mode |= S_IFCHR;
	else if (vp->v_type == VLNK)
		mode |= S_IFLNK;
	else if (vp->v_type == VFIFO)
		mode |= S_IFIFO;
	else if (vp->v_type == VSOCK)
		mode |= S_IFSOCK;

	vfsnode_invalidate_stat_cache(np);
	error = sfprov_set_attr(np->vboxfsmp->sf_handle, np->sf_path,
	    mode, vap->va_atime, vap->va_mtime, vap->va_ctime);
#if 0
	if (error == ENOENT)
		sfnode_make_stale(np);
#endif
	if (vap->va_flags != (u_long)VNOVAL || vap->va_uid != (uid_t)VNOVAL ||
	    vap->va_gid != (gid_t)VNOVAL || vap->va_atime.tv_sec != VNOVAL ||
	    vap->va_mtime.tv_sec != VNOVAL || vap->va_mode != (mode_t)VNOVAL)
		return (EROFS);
	if (vap->va_size != (u_quad_t)VNOVAL) {
		switch (vp->v_type) {
		case VDIR:
			return (EISDIR);
		case VLNK:
		case VREG:
			return (EROFS);
		case VCHR:
		case VBLK:
		case VSOCK:
		case VFIFO:
		case VNON:
		case VBAD:
		case VMARKER:
			return (0);
		}
	}
	return (error);
}

#define blkoff(vboxfsmp, loc)	((loc) & (vboxfsmp)->bmask)

static int
vboxfs_read(struct vop_read_args *ap)
{
	struct vnode		*vp = ap->a_vp;
	struct uio 		*uio = ap->a_uio;
	struct vboxfs_node	*np = VP_TO_VBOXFS_NODE(vp);
	int			error = 0;
	uint32_t		bytes;
	uint32_t		done;
	unsigned long		offset;
	ssize_t			total;
	void			*tmpbuf;

	if (vp->v_type == VDIR)
		return (EISDIR);
	if (vp->v_type != VREG)
		return (EINVAL);
#if 0
	if (uio->uio_loffset >= MAXOFFSET_T) {
		proc_t *p = ttoproc(curthread);
		(void) rctl_action(rctlproc_legacy[RLIMIT_FSIZE], p->p_rctls,
		    p, RCA_UNSAFE_SIGINFO);
		return (EFBIG);
	}
	if (uio->uio_loffset < 0)
		return (EINVAL);
#endif
	total = uio->uio_resid;
	if (total == 0)
		return (0);

	if (np->sf_file == NULL)
		return (ENXIO);

	/*
	 * XXXGONZO: this is just to get things working
	 * should be optimized
	 */
	tmpbuf = contigmalloc(PAGE_SIZE, M_DEVBUF, M_WAITOK, 0, ~0, PAGE_SIZE, 0);
	if (tmpbuf == 0)
		return (ENOMEM);

	do {
		offset = uio->uio_offset;
		done = bytes = min(PAGE_SIZE, uio->uio_resid);
		error = sfprov_read(np->sf_file, tmpbuf,
		    offset, &done, 0);
		if (error == 0 && done > 0)
			error = uiomove(tmpbuf, done, uio);
	} while (error == 0 && uio->uio_resid > 0 && done > 0);

	contigfree(tmpbuf, PAGE_SIZE, M_DEVBUF);

	/* a partial read is never an error */
	if (total != uio->uio_resid)
		error = 0;
	return (error);
}

static int
vboxfs_write(struct vop_write_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_create(struct vop_create_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_remove(struct vop_remove_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_rename(struct vop_rename_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_link(struct vop_link_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_symlink(struct vop_symlink_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_mknod(struct vop_mknod_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_mkdir(struct vop_mkdir_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_rmdir(struct vop_rmdir_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_readdir(struct vop_readdir_args *ap)
{
	int *eofp = ap->a_eofflag;
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct vboxfs_node *dir = VP_TO_VBOXFS_NODE(vp);
	struct vboxfs_node *node;
	struct sffs_dirent *dirent = NULL;
	sffs_dirents_t *cur_buf;
	off_t offset = 0;
	off_t orig_off = uio->uio_offset;
	int error = 0;
	int dummy_eof;

	if (vp->v_type != VDIR)
		return (ENOTDIR);

	if (eofp == NULL)
		eofp = &dummy_eof;
	*eofp = 0;

	/*
	 * Get the directory entry names from the host. This gets all
	 * entries. These are stored in a linked list of sffs_dirents_t
	 * buffers, each of which contains a list of dirent64_t's.
	 */
	if (dir->sf_dir_list == NULL) {
		error = sfprov_readdir(dir->vboxfsmp->sf_handle, dir->sf_path,
		    &dir->sf_dir_list);
		if (error != 0)
			goto done;
	}

	/*
	 * Validate and skip to the desired offset.
	 */
	cur_buf = dir->sf_dir_list;
	offset = 0;

	while (cur_buf != NULL && offset + cur_buf->sf_len <= uio->uio_offset) {
		offset += cur_buf->sf_len;
		cur_buf = cur_buf->sf_next;
	}

	if (cur_buf == NULL && offset != uio->uio_offset) {
		error = EINVAL;
		goto done;
	}

	if (cur_buf != NULL && offset != uio->uio_offset) {
		off_t off = offset;
		int step;
		dirent = &cur_buf->sf_entries[0];

		while (off < uio->uio_offset) {
                        if (dirent->sf_off == uio->uio_offset)
                                break;
			step = sizeof(struct sffs_dirent) + dirent->sf_entry.d_reclen;
			dirent = (struct sffs_dirent *) (((char *) dirent) + step);
			off += step;
		}

		if (off >= uio->uio_offset) {
			error = EINVAL;
			goto done;
		}
	}

	offset = uio->uio_offset - offset;

	/*
	 * Lookup each of the names, so that we have ino's, and copy to
	 * result buffer.
	 */
	while (cur_buf != NULL) {
		if (offset >= cur_buf->sf_len) {
			cur_buf = cur_buf->sf_next;
			offset = 0;
			continue;
		}

		dirent = (struct sffs_dirent *)
		    (((char *) &cur_buf->sf_entries[0]) + offset);
		if (dirent->sf_entry.d_reclen > uio->uio_resid)
			break;

		if (strcmp(dirent->sf_entry.d_name, ".") == 0) {
			node = dir;
		} else if (strcmp(dirent->sf_entry.d_name, "..") == 0) {
			node = dir->sf_parent;
			if (node == NULL)
				node = dir;
		} else {
#if 0
			node = vsfnode_lookup(dir, dirent->sf_entry.d_name, VNON,
			    0, &dirent->sf_stat, vsfnode_cur_time_usec(), NULL);
			if (node == NULL)
				panic("sffs_readdir() lookup failed");
#endif
		}

		if (node)
			dirent->sf_entry.d_fileno = node->sf_ino;
		else
			dirent->sf_entry.d_fileno = 0xdeadbeef;

		error = uiomove(&dirent->sf_entry, dirent->sf_entry.d_reclen, uio);
		if (error != 0)
			break;

		uio->uio_offset = dirent->sf_off;
		offset += sizeof(struct sffs_dirent) + dirent->sf_entry.d_reclen;
	}

	if (error == 0 && cur_buf == NULL)
		*eofp = 1;
done:
	if (error != 0)
		uio->uio_offset = orig_off;
	return (error);
}

static int
vboxfs_readlink(struct vop_readlink_args *v)
{
	struct vnode *vp = v->a_vp;
	struct uio *uio = v->a_uio;

	int error;
	struct vboxfs_node *np;
	void *tmpbuf;

	MPASS(uio->uio_offset == 0);
	MPASS(vp->v_type == VLNK);

	np = VP_TO_VBOXFS_NODE(vp);

	tmpbuf = contigmalloc(MAXPATHLEN, M_DEVBUF, M_WAITOK, 0, ~0, 1, 0);
	if (tmpbuf == NULL)
		return (ENOMEM);

	error = sfprov_readlink(np->vboxfsmp->sf_handle, np->sf_path, tmpbuf,
	    MAXPATHLEN);
	if (error)
		goto done;

	error = uiomove(tmpbuf, strlen(tmpbuf), uio);

done:
	if (tmpbuf)
		contigfree(tmpbuf, MAXPATHLEN, M_DEVBUF);
	return (error);
}

static int
vboxfs_fsync(struct vop_fsync_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_print(struct vop_print_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vboxfs_node *np;

	np = VP_TO_VBOXFS_NODE(vp);

	if (np == NULL) {
		printf("No vboxfs_node data\n");
		return (0);
	}

	printf("\tpath = %s, parent = %p", np->sf_path,
	    np->sf_parent ? np->sf_parent : NULL);
	printf("\n");
	return (0);
}

static int
vboxfs_pathconf(struct vop_pathconf_args *ap)
{
	register_t *retval = ap->a_retval;
	int error = 0;

	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*retval = 65535;
		break;
	case _PC_NAME_MAX:
		*retval = NAME_MAX;
		break;
	case _PC_PATH_MAX:
		*retval = PATH_MAX; 
		break;
	default:
		error = EINVAL;
		break;
	}
	return (error);
}

/*
 * File specific ioctls.
 */
static int
vboxfs_ioctl(struct vop_ioctl_args *ap)
{
	return (ENOTTY);
}

static int
vboxfs_getextattr(struct vop_getextattr_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_advlock(struct vop_advlock_args *ap)
{
	return (EOPNOTSUPP);
}

/*
 * Lookup an entry in a directory and create a new vnode if found.
 */	
static int 
vboxfs_lookup(struct vop_cachedlookup_args /* {
		struct vnodeop_desc *a_desc;
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
	} */ *ap)
{
	struct 	componentname *cnp = ap->a_cnp;
	struct 	vnode *dvp = ap->a_dvp;		/* the directory vnode */
	char	*nameptr = cnp->cn_nameptr;	/* the name of the file or directory */
	struct	vnode **vpp = ap->a_vpp;	/* the vnode we found or NULL */
	struct  vnode *tdp = NULL;
	struct 	vboxfs_node *node = VP_TO_VBOXFS_NODE(dvp);
	struct 	vboxfs_mnt *vboxfsmp = node->vboxfsmp;
	u_long  nameiop = cnp->cn_nameiop;
	u_long 	flags = cnp->cn_flags;
	sffs_stat_t	*stat, tmp_stat;
	//long 	namelen;
	ino_t 	id = 0;
	int 	ltype, type, error = 0;
	int 	lkflags = cnp->cn_lkflags;	
	char	*fullpath = NULL;

	error = ENOENT;
	if (cnp->cn_flags & ISDOTDOT) {
		error = vn_vget_ino_gen(dvp, vboxfs_vn_get_ino_alloc,
		    node->sf_parent, cnp->cn_lkflags, vpp);
		error = ENOENT;
		if (error != 0)
			goto out;
		
	} else if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		VREF(dvp);
		*vpp = dvp;
		error = 0;
	} else {
		mode_t m;
		type = VNON;
		stat = &tmp_stat;
		fullpath = sfnode_construct_path(node, cnp->cn_nameptr);
		error = sfprov_get_attr(node->vboxfsmp->sf_handle,
		    fullpath, stat);
		// stat_time = vsfnode_cur_time_usec();

		m = stat->sf_mode;
		if (error != 0)
			error = ENOENT;
		else if (S_ISDIR(m))
			type = VDIR;
		else if (S_ISREG(m))
			type = VREG;
		else if (S_ISLNK(m))
			type = VLNK;
		if (error == 0) {
			struct vboxfs_node *unode;
			error = vboxfs_alloc_node(vboxfsmp->sf_vfsp, vboxfsmp, fullpath, type, 0,
	    			0, 0755, node, &unode);
			error = vboxfs_alloc_vp(vboxfsmp->sf_vfsp, unode, cnp->cn_lkflags, vpp);
		}
	}

	if ((cnp->cn_flags & MAKEENTRY) != 0)
		cache_enter(dvp, *vpp, cnp);
out:
	if (fullpath)
		free(fullpath, M_VBOXVFS);
	return (error);
}

static int
vboxfs_inactive(struct vop_inactive_args *ap)
{
   	return (0);
}

static int
vboxfs_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vp;
	struct vboxfs_node *node;
	struct 	vboxfs_mnt *vboxfsmp;

	vp = ap->a_vp;
	node = VP_TO_VBOXFS_NODE(vp);
	vboxfsmp = node->vboxfsmp;

	vnode_destroy_vobject(vp);
	vp->v_object = NULL;
	cache_purge(vp);

	VBOXFS_NODE_LOCK(node);
	VBOXFS_ASSERT_ELOCKED(node);
	vboxfs_free_vp(vp);

	/* If the node referenced by this vnode was deleted by the user,
	 * we must free its associated data structures (now that the vnode
	 * is being reclaimed). */
	if ((node->sf_vpstate & VBOXFS_VNODE_ALLOCATING) == 0) {
		node->sf_vpstate = VBOXFS_VNODE_DOOMED;
		VBOXFS_NODE_UNLOCK(node);
		vboxfs_free_node(vboxfsmp, node);
	} else
		VBOXFS_NODE_UNLOCK(node);

	MPASS(vp->v_data == NULL);

	return (0);
}

static int
vboxfs_vptofh(struct vop_vptofh_args *ap)
{

	return (EOPNOTSUPP);
}

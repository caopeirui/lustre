/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2001-2003 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/smp_lock.h>
#include <linux/quotaops.h>

#define DEBUG_SUBSYSTEM S_LLITE

#include <linux/obd_support.h>
#include <linux/lustre_lite.h>
#include <linux/lustre_idl.h>
#include <linux/lustre_dlm.h>
#include <linux/lustre_version.h>

#include "llite_internal.h"

/* should NOT be called with the dcache lock, see fs/dcache.c */
static void ll_release(struct dentry *de)
{
        struct ll_dentry_data *lld;
        ENTRY;
        LASSERT(de != NULL);
        lld = ll_d2d(de);
        LASSERT(lld != NULL);
        LASSERT(lld->lld_cwd_count == 0);
        LASSERT(lld->lld_mnt_count == 0);
        OBD_FREE(de->d_fsdata, sizeof(struct ll_dentry_data));

        EXIT;
}

/* should NOT be called with the dcache lock, see fs/dcache.c */
static int ll_ddelete(struct dentry *de)
{
        ENTRY;
        LASSERT(de);
        CDEBUG(D_DENTRY, "%s dentry %*s (%p, parent %p, inode %p) %s%s\n",
               (de->d_flags & DCACHE_LUSTRE_INVALID ? "deleting" : "keeping"),
               de->d_name.len, de->d_name.name, de, de->d_parent, de->d_inode,
               d_unhashed(de) ? "" : "hashed,",
               list_empty(&de->d_subdirs) ? "" : "subdirs");
        RETURN(0);
}

void ll_set_dd(struct dentry *de)
{
        ENTRY;
        LASSERT(de != NULL);

        CDEBUG(D_DENTRY, "ldd on dentry %*s (%p) parent %p inode %p refc %d\n",
               de->d_name.len, de->d_name.name, de, de->d_parent, de->d_inode,
               atomic_read(&de->d_count));
        lock_kernel();
        if (de->d_fsdata == NULL) {
                OBD_ALLOC(de->d_fsdata, sizeof(struct ll_dentry_data));
        }
        unlock_kernel();

        EXIT;
}

void ll_intent_drop_lock(struct lookup_intent *it)
{
        struct lustre_handle *handle;

        if (it->it_op && it->d.lustre.it_lock_mode) {
                handle = (struct lustre_handle *)&it->d.lustre.it_lock_handle;
                CDEBUG(D_DLMTRACE, "releasing lock with cookie "LPX64
                       " from it %p\n", handle->cookie, it);
                ldlm_lock_decref(handle, it->d.lustre.it_lock_mode);

                /* bug 494: intent_release may be called multiple times, from
                 * this thread and we don't want to double-decref this lock */
                it->d.lustre.it_lock_mode = 0;
        }
}

void ll_intent_release(struct lookup_intent *it)
{
        ENTRY;

        ll_intent_drop_lock(it);
        it->it_magic = 0;
        it->it_op_release = 0;
        it->d.lustre.it_disposition = 0;
        it->d.lustre.it_data = NULL;
        EXIT;
}

void ll_unhash_aliases(struct inode *inode)
{
        struct list_head *tmp, *head;
        struct ll_sb_info *sbi;
        ENTRY;

        if (inode == NULL) {
                CERROR("unexpected NULL inode, tell phil\n");
                return;
        }

        CDEBUG(D_INODE, "marking dentries for ino %lu/%u(%p) invalid\n",
               inode->i_ino, inode->i_generation, inode);

        sbi = ll_i2sbi(inode);
        head = &inode->i_dentry;
restart:
        spin_lock(&dcache_lock);
        tmp = head;
        while ((tmp = tmp->next) != head) {
                struct dentry *dentry = list_entry(tmp, struct dentry, d_alias);
                if (atomic_read(&dentry->d_count) == 0) {
                        CDEBUG(D_DENTRY, "deleting dentry %*s (%p) parent %p "
                               "inode %p\n", dentry->d_name.len,
                               dentry->d_name.name, dentry, dentry->d_parent,
                               dentry->d_inode);
                        dget_locked(dentry);
                        __d_drop(dentry);
                        spin_unlock(&dcache_lock);
                        dput(dentry);
                        goto restart;
                } else if (!(dentry->d_flags & DCACHE_LUSTRE_INVALID)) {
                        CDEBUG(D_DENTRY, "unhashing dentry %*s (%p) parent %p "
                               "inode %p refc %d\n", dentry->d_name.len,
                               dentry->d_name.name, dentry, dentry->d_parent,
                               dentry->d_inode, atomic_read(&dentry->d_count));
                        hlist_del_init(&dentry->d_hash);
                        dentry->d_flags |= DCACHE_LUSTRE_INVALID;
                        hlist_add_head(&dentry->d_hash,
                                       &sbi->ll_orphan_dentry_list);
                }
        }
        spin_unlock(&dcache_lock);
        EXIT;
}

extern struct dentry *ll_find_alias(struct inode *, struct dentry *);

int revalidate_it_finish(struct ptlrpc_request *request, int offset, 
                         struct lookup_intent *it, struct dentry *de)
{
        struct ll_sb_info *sbi;
        int rc = 0;
        ENTRY;

        if (!request)
                RETURN(0);

        if (it_disposition(it, DISP_LOOKUP_NEG))
                RETURN(-ENOENT);

        sbi = ll_i2sbi(de->d_inode);
        rc = ll_prep_inode(sbi->ll_lov_exp, sbi->ll_lmv_exp,
                           &de->d_inode, request, offset, NULL);

        RETURN(rc);
}

void ll_lookup_finish_locks(struct lookup_intent *it, struct dentry *dentry)
{
        LASSERT(it != NULL);
        LASSERT(dentry != NULL);

        if (it->d.lustre.it_lock_mode && dentry->d_inode != NULL) {
                struct inode *inode = dentry->d_inode;
                CDEBUG(D_DLMTRACE, "setting l_data to inode %p (%lu/%u)\n",
                       inode, inode->i_ino, inode->i_generation);
                mdc_set_lock_data(NULL, &it->d.lustre.it_lock_handle, inode);
        }

        /* drop lookup or getattr locks immediately */
        if (it->it_op == IT_LOOKUP || it->it_op == IT_GETATTR || it->it_op == IT_CHDIR) {
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
                /* on 2.6 there are situation when several lookups and
                 * revalidations may be requested during single operation.
                 * therefore, we don't release intent here -bzzz */
                ll_intent_drop_lock(it);
#else
                ll_intent_release(it);
#endif
        }
}

void ll_frob_intent(struct lookup_intent **itp, struct lookup_intent *deft)
{
        struct lookup_intent *it = *itp;
        
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
        if (it) {
                LASSERTF(it->it_magic == INTENT_MAGIC, "bad intent magic: %x\n",
                         it->it_magic);
        }
#endif

        if (!it || it->it_op == IT_GETXATTR)
                it = *itp = deft;

        it->it_op_release = ll_intent_release;
}

int ll_revalidate_it(struct dentry *de, int flags, struct nameidata *nd,
                     struct lookup_intent *it)
{
        int rc;
        struct lustre_id pid;
        struct lustre_id cid;
        struct it_cb_data icbd;
        struct ptlrpc_request *req = NULL;
        struct lookup_intent lookup_it = { .it_op = IT_LOOKUP };
        struct obd_export *exp;

        ENTRY;
        CDEBUG(D_VFSTRACE, "VFS Op:name=%s,intent=%s\n", de->d_name.name,
               LL_IT2STR(it));

        /* Cached negative dentries are unsafe for now - look them up again */
        if (de->d_inode == NULL)
                RETURN(0);

        CDEBUG(D_INODE, "revalidate 0x%p: %*s -> %lu/%lu\n",
               de, de->d_name.len, de->d_name.name,
               (unsigned long) de->d_inode->i_ino,
               (unsigned long) de->d_inode->i_generation);

        exp = ll_i2lmvexp(de->d_inode);
        ll_inode2id(&pid, de->d_parent->d_inode);
        ll_inode2id(&cid, de->d_inode);
        icbd.icbd_parent = de->d_parent->d_inode;
        icbd.icbd_childp = &de;

        /*
         * never execute intents for mount points. Attributes will be fixed up
         * in ll_inode_revalidate_it().
         */
        if (d_mountpoint(de))
                RETURN(1);

        if (nd != NULL)
                nd->mnt->mnt_last_used = jiffies;

        ll_frob_intent(&it, &lookup_it);
        LASSERT(it);

        if (it->it_op == IT_GETATTR) { /* We need to check for LOOKUP lock
                                          as well */
                rc = md_intent_lock(exp, &pid, de->d_name.name,
                                    de->d_name.len, NULL, 0, &cid, &lookup_it,
                                    flags, &req, ll_mdc_blocking_ast);
                /* If there was no lookup lock, no point in even checking for
                   UPDATE lock */
                if (!rc) {
                        it = &lookup_it;
                        GOTO(out, rc);
                }
                if (it_disposition(&lookup_it, DISP_LOOKUP_NEG)) {
                        ll_intent_release(&lookup_it);
                        it = &lookup_it;
                        GOTO(out, rc = 0);
                }

                if (req)
                        ptlrpc_req_finished(req);
                req = NULL;
                ll_lookup_finish_locks(&lookup_it, de);
        }

        /* at this point fid should be valid. */
        LASSERT(id_fid(&cid) != 0);
        
        rc = md_intent_lock(exp, &pid, de->d_name.name, de->d_name.len,
                            NULL, 0, &cid, it, flags, &req,
                            ll_mdc_blocking_ast);
        
        /* If req is NULL, then mdc_intent_lock only tried to do a lock match;
         * if all was well, it will return 1 if it found locks, 0 otherwise. */
        if (req == NULL && rc >= 0)
                GOTO(out, rc);

        if (rc < 0) {
                if (rc != -ESTALE) {
                        CDEBUG(D_INFO, "ll_intent_lock: rc %d : it->it_status "
                               "%d\n", rc, it->d.lustre.it_status);
                }
                GOTO(out, rc = 0);
        }

        rc = revalidate_it_finish(req, 1, it, de);
        if (rc != 0) {
                ll_intent_release(it);
                GOTO(out, rc = 0);
        }
        rc = 1;

        /* unfortunately ll_intent_lock may cause a callback and revoke our
           dentry */
        spin_lock(&dcache_lock);
        hlist_del_init(&de->d_hash);
        __d_rehash(de, 0);
        spin_unlock(&dcache_lock);

        GOTO(out, rc);
 out:
        if (req != NULL && rc == 1)
                ptlrpc_req_finished(req);
        if (rc == 0) {
                ll_unhash_aliases(de->d_inode);
                return rc;
        }

        CDEBUG(D_DENTRY, "revalidated dentry %*s (%p) parent %p "
               "inode %p refc %d\n", de->d_name.len,
               de->d_name.name, de, de->d_parent, de->d_inode,
               atomic_read(&de->d_count));
        ll_lookup_finish_locks(it, de);
        de->d_flags &= ~DCACHE_LUSTRE_INVALID;
        if (!((de->d_inode->i_mode & S_ISUID) &&S_ISDIR(de->d_inode->i_mode)) ||
            !(flags & LOOKUP_CONTINUE || (it->it_op & (IT_CHDIR | IT_OPEN))))
                return rc;

        if (nd)
                (void)ll_dir_process_mount_object(de, nd->mnt);
        return rc;
}

/*static*/ void ll_pin(struct dentry *de, struct vfsmount *mnt, int flag)
{
        struct inode *inode= de->d_inode;
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ll_dentry_data *ldd = ll_d2d(de);
        struct obd_client_handle *handle;
        int rc = 0;
        ENTRY;
        LASSERT(ldd);

        lock_kernel();
        /* Strictly speaking this introduces an additional race: the
         * increments should wait until the rpc has returned.
         * However, given that at present the function is void, this
         * issue is moot. */
        if (flag == 1 && (++ldd->lld_mnt_count) > 1) {
                unlock_kernel();
                EXIT;
                return;
        }

        if (flag == 0 && (++ldd->lld_cwd_count) > 1) {
                unlock_kernel();
                EXIT;
                return;
        }
        unlock_kernel();

        handle = (flag) ? &ldd->lld_mnt_och : &ldd->lld_cwd_och;
        rc = obd_pin(sbi->ll_lmv_exp, inode->i_ino, inode->i_generation,
                     inode->i_mode & S_IFMT, handle, flag);

        if (rc) {
                lock_kernel();
                memset(handle, 0, sizeof(*handle));
                if (flag == 0)
                        ldd->lld_cwd_count--;
                else
                        ldd->lld_mnt_count--;
                unlock_kernel();
        }

        EXIT;
        return;
}

/*static*/ void ll_unpin(struct dentry *de, struct vfsmount *mnt, int flag)
{
        struct ll_sb_info *sbi = ll_i2sbi(de->d_inode);
        struct ll_dentry_data *ldd = ll_d2d(de);
        struct obd_client_handle handle;
        int count, rc = 0;
        ENTRY;
        LASSERT(ldd);

        lock_kernel();
        /* Strictly speaking this introduces an additional race: the
         * increments should wait until the rpc has returned.
         * However, given that at present the function is void, this
         * issue is moot. */
        handle = (flag) ? ldd->lld_mnt_och : ldd->lld_cwd_och;
        if (handle.och_magic != OBD_CLIENT_HANDLE_MAGIC) {
                /* the "pin" failed */
                unlock_kernel();
                EXIT;
                return;
        }

        if (flag)
                count = --ldd->lld_mnt_count;
        else
                count = --ldd->lld_cwd_count;
        unlock_kernel();

        if (count != 0) {
                EXIT;
                return;
        }

        rc = obd_unpin(sbi->ll_lmv_exp, &handle, flag);
        EXIT;
        return;
}

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
static int ll_revalidate_nd(struct dentry *dentry, struct nameidata *nd)
{
        int rc;
        ENTRY;

        if (nd && nd->flags & LOOKUP_LAST && !(nd->flags & LOOKUP_LINK_NOTLAST))
                rc = ll_revalidate_it(dentry, nd->flags, nd, &nd->intent);
        else
                rc = ll_revalidate_it(dentry, 0, nd, NULL);

        RETURN(rc);
}
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
static void ll_dentry_iput(struct dentry *dentry, struct inode *inode)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct lustre_id parent, child;

        LASSERT(dentry->d_parent && dentry->d_parent->d_inode);
        ll_inode2id(&parent, dentry->d_parent->d_inode);
        ll_inode2id(&child, inode);
        md_change_cbdata_name(sbi->ll_lmv_exp, &parent,
                              (char *)dentry->d_name.name, 
                              dentry->d_name.len, &child, 
                              null_if_equal, inode);
        iput(inode);
}
#else
static void ll_dentry_iput(struct dentry *dentry, struct inode *inode)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ll_fid parent, child;

        LASSERT(dentry->d_parent && dentry->d_parent->d_inode);
        ll_inode2fid(&parent, dentry->d_parent->d_inode);
        ll_inode2fid(&child, inode);
        md_change_cbdata_name(sbi->ll_mdc_exp, &parent,
                              (char *)dentry->d_name.name, 
                              dentry->d_name.len, &child, 
                              null_if_equal, inode);
        iput(inode);
}
#endif

struct dentry_operations ll_d_ops = {
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
        .d_revalidate = ll_revalidate_nd,
#else
        .d_revalidate_it = ll_revalidate_it,
#endif
        .d_release = ll_release,
        .d_iput = ll_dentry_iput,
        .d_delete = ll_ddelete,
#if 0
        .d_pin = ll_pin,
        .d_unpin = ll_unpin,
#endif
};

/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/fid/fid_handler.c
 *  Lustre Sequence Manager
 *
 *  Copyright (c) 2006 Cluster File Systems, Inc.
 *   Author: Yury Umanets <umka@clusterfs.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_FID

#ifdef __KERNEL__
# include <libcfs/libcfs.h>
# include <linux/module.h>
#else /* __KERNEL__ */
# include <liblustre.h>
#endif

#include <obd.h>
#include <obd_class.h>
#include <dt_object.h>
#include <md_object.h>
#include <obd_support.h>
#include <lustre_req_layout.h>
#include <lustre_fid.h>
#include "fid_internal.h"

/* client seq mgr interface */
static int 
seq_client_rpc(struct lu_client_seq *seq, 
               struct lu_range *range,
               __u32 opc)
{
        int repsize = sizeof(struct lu_range);
        int rc, reqsize = sizeof(__u32);
        struct ptlrpc_request *req;
        struct lu_range *ran;
        __u32 *op;
        ENTRY;

        req = ptlrpc_prep_req(class_exp2cliimp(seq->seq_exp), 
			      LUSTRE_MDS_VERSION, SEQ_QUERY,
			      1, &reqsize, NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        op = lustre_msg_buf(req->rq_reqmsg, 0, sizeof(*op));
        *op = opc;

        req->rq_replen = lustre_msg_size(1, &repsize);
        req->rq_request_portal = MDS_SEQ_PORTAL;
        rc = ptlrpc_queue_wait(req);
        if (rc)
                GOTO(out_req, rc);

        ran = lustre_swab_repbuf(req, 0, sizeof(*ran),
                                 lustre_swab_lu_range);

        if (ran == NULL) {
                CERROR("invalid range is returned\n");
                GOTO(out_req, rc = -EPROTO);
        }
        *range = *ran;
        EXIT;
out_req:
        ptlrpc_req_finished(req); 
        return rc;
}

/* request sequence-controller node to allocate new super-sequence. */
int
seq_client_alloc_super(struct lu_client_seq *seq)
{
        int rc;
        ENTRY;

        LASSERT(seq->seq_flags & LUSTRE_CLI_SEQ_SERVER);
        rc = seq_client_rpc(seq, &seq->seq_cl_range,
                            SEQ_ALLOC_SUPER);
        if (rc == 0) {
                CDEBUG(D_INFO, "SEQ-MGR(cli): allocated super-sequence "
                       "["LPX64"-"LPX64"]\n", seq->seq_cl_range.lr_start,
                       seq->seq_cl_range.lr_end);
        }
        RETURN(rc);
}
EXPORT_SYMBOL(seq_client_alloc_super);

/* request sequence-controller node to allocate new meta-sequence. */
int
seq_client_alloc_meta(struct lu_client_seq *seq)
{
        int rc;
        ENTRY;

        LASSERT(seq->seq_flags & LUSTRE_CLI_SEQ_CLIENT);
        rc = seq_client_rpc(seq, &seq->seq_cl_range,
                            SEQ_ALLOC_META);
        if (rc == 0) {
                CDEBUG(D_INFO, "SEQ-MGR(cli): allocated meta-sequence "
                       "["LPX64"-"LPX64"]\n", seq->seq_cl_range.lr_start,
                       seq->seq_cl_range.lr_end);
        }
        RETURN(rc);
}
EXPORT_SYMBOL(seq_client_alloc_meta);

/* allocate new sequence for client (llite or MDC are expected to use this) */
int
seq_client_alloc_seq(struct lu_client_seq *seq, __u64 *seqnr)
{
        int rc;
        ENTRY;

        down(&seq->seq_sem);

        LASSERT(seq->seq_flags & LUSTRE_CLI_SEQ_CLIENT);
        LASSERT(range_is_sane(&seq->seq_cl_range));

        /* if we still have free sequences in meta-sequence we allocate new seq
         * from given range. */
        if (seq->seq_cl_range.lr_end > seq->seq_cl_range.lr_start) {
                *seqnr = seq->seq_cl_range.lr_start;
                seq->seq_cl_range.lr_start += 1;
                rc = 0;
        } else {
                /* meta-sequence is exhausted, request MDT to allocate new
                 * meta-sequence for us. */
                rc = seq_client_alloc_meta(seq);
                if (rc) {
                        CERROR("can't allocate new meta-sequence, "
                               "rc %d\n", rc);
                }
                
                *seqnr = seq->seq_cl_range.lr_start;
                seq->seq_cl_range.lr_start += 1;
        }
        up(&seq->seq_sem);

        if (rc == 0) {
                CDEBUG(D_INFO, "SEQ-MGR(cli): allocated sequence "
                       "["LPX64"]\n", *seqnr);
        }
        RETURN(rc);
}
EXPORT_SYMBOL(seq_client_alloc_seq);

int
seq_client_alloc_fid(struct lu_client_seq *seq, struct lu_fid *fid)
{
        int rc;
        ENTRY;

        LASSERT(fid != NULL);
        LASSERT(fid_is_sane(&seq->seq_fid));
        LASSERT(seq->seq_flags & LUSTRE_CLI_SEQ_CLIENT);

        down(&seq->seq_sem);
        if (fid_oid(&seq->seq_fid) < LUSTRE_SEQ_WIDTH) {
                *fid = seq->seq_fid;
                seq->seq_fid.f_oid += 1;
                rc = 0;
        } else {
                __u64 seqnr = 0;
                
                rc = seq_client_alloc_seq(seq, &seqnr);
                if (rc) {
                        CERROR("can't allocate new sequence, "
                               "rc %d\n", rc);
                        GOTO(out, rc);
                } else {
                        seq->seq_fid.f_oid = LUSTRE_FID_INIT_OID;
                        seq->seq_fid.f_seq = seqnr;
                        seq->seq_fid.f_ver = 0;
                        
                        *fid = seq->seq_fid;
                        seq->seq_fid.f_oid += 1;
                }
        }
        LASSERT(fid_is_sane(fid));
        
        CDEBUG(D_INFO, "SEQ-MGR(cli): allocated FID "DFID3"\n",
               PFID3(fid));

        EXIT;
out:
        up(&seq->seq_sem);
        return rc;
}
EXPORT_SYMBOL(seq_client_alloc_fid);

int 
seq_client_init(struct lu_client_seq *seq, 
                struct obd_export *exp,
                int flags)
{
        int rc;
        ENTRY;

        LASSERT(flags & (LUSTRE_CLI_SEQ_CLIENT |
                         LUSTRE_CLI_SEQ_SERVER));

        seq->seq_flags = flags;
        fid_zero(&seq->seq_fid);
        sema_init(&seq->seq_sem, 1);
        
        seq->seq_cl_range.lr_end = 0;
        seq->seq_cl_range.lr_start = 0;
	
        if (exp != NULL)
                seq->seq_exp = class_export_get(exp);

        if (seq->seq_flags & LUSTRE_CLI_SEQ_CLIENT) {
                __u64 seqnr = 0;
                
                /* client (llite or MDC) init case, we need new sequence from
                 * MDT. This will allocate new meta-sequemce first, because seq
                 * range in init state and looks the same as exhausted. */
                rc = seq_client_alloc_seq(seq, &seqnr);
                if (rc) {
                        CERROR("can't allocate new sequence, rc %d\n", rc);
                        GOTO(out, rc);
                } else {
                        seq->seq_fid.f_oid = LUSTRE_FID_INIT_OID;
                        seq->seq_fid.f_seq = seqnr;
                        seq->seq_fid.f_ver = 0;
                }

                LASSERT(fid_is_sane(&seq->seq_fid));
        } else {
                /* check if this is controller node is trying to init client. */
                if (seq->seq_exp) {
                        /* MDT uses client seq manager to talk to sequence
                         * controller, and thus, we need super-sequence. */
                        rc = seq_client_alloc_super(seq);
                } else {
                        rc = 0;
                }
        }

        EXIT;
out:
        if (rc)
                seq_client_fini(seq);
        else
                CDEBUG(D_INFO, "Client Sequence Manager initialized\n");
        return rc;
}
EXPORT_SYMBOL(seq_client_init);

void seq_client_fini(struct lu_client_seq *seq)
{
        ENTRY;
        if (seq->seq_exp != NULL) {
                class_export_put(seq->seq_exp);
                seq->seq_exp = NULL;
        }
        CDEBUG(D_INFO, "Client Sequence Manager finalized\n");
        EXIT;
}
EXPORT_SYMBOL(seq_client_fini);

#ifdef __KERNEL__
/* server side seq mgr stuff */
static const struct lu_range LUSTRE_SEQ_SUPER_INIT = {
        LUSTRE_SEQ_SPACE_START,
        LUSTRE_SEQ_SPACE_LIMIT
};

static const struct lu_range LUSTRE_SEQ_META_INIT = {
        0,
        0
};

static int
seq_server_write_state(struct lu_server_seq *seq,
                       const struct lu_context *ctx)
{
	int rc = 0;
	ENTRY;

	/* XXX: here should be calling struct dt_device methods to write
	 * sequence state to backing store. */

	RETURN(rc);
}

static int
seq_server_read_state(struct lu_server_seq *seq,
                      const struct lu_context *ctx)
{
	int rc = -ENODATA;
	ENTRY;
	
	/* XXX: here should be calling struct dt_device methods to read the
	 * sequence state from backing store. */

	RETURN(rc);
}

static int
seq_server_alloc_super(struct lu_server_seq *seq,
                       struct lu_range *range)
{
        struct lu_range *ss_range = &seq->seq_ss_range;
        int rc;
        ENTRY;

        if (ss_range->lr_end - ss_range->lr_start < LUSTRE_SEQ_SUPER_CHUNK) {
                CWARN("super-sequence is going to exhauste soon. "
                      "Only can allocate "LPU64" sequences\n",
                      ss_range->lr_end - ss_range->lr_start);
                *range = *ss_range;
                ss_range->lr_start = ss_range->lr_end;
                rc = 0;
        } else if (ss_range->lr_start >= ss_range->lr_end) {
                CERROR("super-sequence is exhausted\n");
                rc = -ENOSPC;
        } else {
                range->lr_start = ss_range->lr_start;
                ss_range->lr_start += LUSTRE_SEQ_SUPER_CHUNK;
                range->lr_end = ss_range->lr_start;
                rc = 0;
        }

        if (rc == 0) {
                CDEBUG(D_INFO, "SEQ-MGR(srv): allocated super-sequence "
                       "["LPX64"-"LPX64"]\n", range->lr_start,
                       range->lr_end);
        }
        
        RETURN(rc);
}

static int
seq_server_alloc_meta(struct lu_server_seq *seq,
                      struct lu_range *range)
{
        struct lu_range *ms_range = &seq->seq_ms_range;
        int rc;
        ENTRY;

        LASSERT(range_is_sane(ms_range));
        
        /* XXX: here should avoid cascading RPCs using kind of async
         * preallocation when meta-sequence is close to exhausting. */
        if (ms_range->lr_start == ms_range->lr_end) {
                if (seq->seq_flags & LUSTRE_SRV_SEQ_CONTROLLER) {
                        /* allocate new range of meta-sequences to allocate new
                         * meta-sequence from it. */
                        rc = seq_server_alloc_super(seq, ms_range);
                } else {
                        /* request controller to allocate new super-sequence for
                         * us.*/
                        rc = seq_client_alloc_super(seq->seq_cli);
                        if (rc) {
                                CERROR("can't allocate new super-sequence, "
                                       "rc %d\n", rc);
                                RETURN(rc);
                        }

                        /* saving new range into allocation space. */
                        *ms_range = seq->seq_cli->seq_cl_range;
                }

                LASSERT(ms_range->lr_start != 0);
                LASSERT(ms_range->lr_end > ms_range->lr_start);
        } else {
                rc = 0;
        }
        range->lr_start = ms_range->lr_start;
        ms_range->lr_start += LUSTRE_SEQ_META_CHUNK;
        range->lr_end = ms_range->lr_start;

        if (rc == 0) {
                CDEBUG(D_INFO, "SEQ-MGR(srv): allocated meta-sequence "
                       "["LPX64"-"LPX64"]\n", range->lr_start,
                       range->lr_end);
        }

        RETURN(rc);
}

static int
seq_server_handle(struct lu_server_seq *seq,
                  const struct lu_context *ctx, 
                  struct lu_range *range,
                  __u32 opc)
{
        int rc;
        ENTRY;

        down(&seq->seq_sem);

        switch (opc) {
        case SEQ_ALLOC_SUPER:
                rc = seq_server_alloc_super(seq, range);
                break;
        case SEQ_ALLOC_META:
                rc = seq_server_alloc_meta(seq, range);
                break;
        default:
                rc = -EINVAL;
                break;
        }

        if (rc)
                GOTO(out, rc);

        rc = seq_server_write_state(seq, ctx);
        if (rc) {
                CERROR("can't save state, rc = %d\n",
		       rc);
        }

        EXIT;
out:
        up(&seq->seq_sem);
        return rc;
}

static int
seq_req_handle0(const struct lu_context *ctx,
                struct lu_server_seq *seq, 
                struct ptlrpc_request *req) 
{
        int rep_buf_size[2] = { 0, };
        struct req_capsule pill;
        struct lu_range *out;
        int rc = -EPROTO;
        __u32 *opc;
        ENTRY;

        req_capsule_init(&pill, req, RCL_SERVER,
                         rep_buf_size);

        req_capsule_set(&pill, &RQF_SEQ_QUERY);
        req_capsule_pack(&pill);

        opc = req_capsule_client_get(&pill, &RMF_SEQ_OPC);
        if (opc != NULL) {
                out = req_capsule_server_get(&pill, &RMF_SEQ_RANGE);
                if (out == NULL) {
                        CERROR("can't get range buffer\n");
                        GOTO(out_pill, rc= -EPROTO);
                }
                rc = seq_server_handle(seq, ctx, out, *opc);
        } else {
                CERROR("cannot unpack client request\n");
        }

out_pill:
        EXIT;
        req_capsule_fini(&pill);
        return rc;
}

static int 
seq_req_handle(struct ptlrpc_request *req) 
{
        int fail = OBD_FAIL_SEQ_ALL_REPLY_NET;
        const struct lu_context *ctx;
        struct lu_site    *site;
        int rc = -EPROTO;
        ENTRY;

        OBD_FAIL_RETURN(OBD_FAIL_SEQ_ALL_REPLY_NET | OBD_FAIL_ONCE, 0);
        
        ctx = req->rq_svc_thread->t_ctx;
        LASSERT(ctx != NULL);
        LASSERT(ctx->lc_thread == req->rq_svc_thread);
        if (req->rq_reqmsg->opc == SEQ_QUERY) {
                if (req->rq_export != NULL) {
                        struct obd_device *obd;

                        obd = req->rq_export->exp_obd;
                        site = obd->obd_lu_dev->ld_site;
                        LASSERT(site != NULL);
			
                        rc = seq_req_handle0(ctx, site->ls_server_seq, req);
                } else {
                        CERROR("Unconnected request\n");
                        req->rq_status = -ENOTCONN;
                        GOTO(out, rc = -ENOTCONN);
                }
        } else {
                CERROR("Wrong opcode: %d\n",
                       req->rq_reqmsg->opc);
                req->rq_status = -ENOTSUPP;
                rc = ptlrpc_error(req);
                RETURN(rc);
        }

        EXIT;
out:
        target_send_reply(req, rc, fail);
        return 0;
} 

int
seq_server_init(struct lu_server_seq *seq,
                struct lu_client_seq *cli,
                const struct lu_context  *ctx,
                struct dt_device *dev,
                int flags) 
{
        int rc; 
        ENTRY;

        struct ptlrpc_service_conf seq_conf = { 
                .psc_nbufs = MDS_NBUFS, 
                .psc_bufsize = MDS_BUFSIZE, 
                .psc_max_req_size = MDS_MAXREQSIZE,
                .psc_max_reply_size = MDS_MAXREPSIZE,
                .psc_req_portal = MDS_SEQ_PORTAL,
                .psc_rep_portal = MDC_REPLY_PORTAL,
                .psc_watchdog_timeout = SEQ_SERVICE_WATCHDOG_TIMEOUT, 
                .psc_num_threads = SEQ_NUM_THREADS
        };

	LASSERT(dev != NULL);
	LASSERT(cli != NULL);
        
        LASSERT(flags & (LUSTRE_SRV_SEQ_CONTROLLER |
                         LUSTRE_SRV_SEQ_REGULAR));

        seq->seq_dev = dev;
        seq->seq_cli = cli;
        seq->seq_flags = flags;
        sema_init(&seq->seq_sem, 1);

        lu_device_get(&seq->seq_dev->dd_lu_dev);

        /* request backing store for saved sequence info */
        rc = seq_server_read_state(seq, ctx);
        if (rc == -ENODATA) {
                /* first run, no state on disk, init all seqs */
                if (seq->seq_flags & LUSTRE_SRV_SEQ_CONTROLLER) {
                        /* init super seq by start values on sequence-controller
                         * node.*/
                        seq->seq_ss_range = LUSTRE_SEQ_SUPER_INIT;
                } else {
                        /* take super-seq from client seq mgr */
                        LASSERT(range_is_sane(&cli->seq_cl_range));
                        seq->seq_ss_range = cli->seq_cl_range;
                }

                /* init meta-sequence by start values and get ready for
                 * allocating it for clients. */
                seq->seq_ms_range = LUSTRE_SEQ_META_INIT;

                /* save init seq to backing store. */
                rc = seq_server_write_state(seq, ctx);
                if (rc) {
                        CERROR("can't write sequence state, "
                               "rc = %d\n", rc);
			GOTO(out, rc);
                }
        } else if (rc) {
		CERROR("can't read sequence state, rc = %d\n",
		       rc);
		GOTO(out, rc);
	}

	seq->seq_service =  ptlrpc_init_svc_conf(&seq_conf,
						 seq_req_handle,
						 LUSTRE_SEQ0_NAME,
						 seq->seq_proc_entry, 
						 NULL); 
	if (seq->seq_service != NULL)
		rc = ptlrpc_start_threads(NULL, seq->seq_service,
					  LUSTRE_SEQ0_NAME); 
	else 
		rc = -ENOMEM; 

	EXIT;

out:
	if (rc)
		seq_server_fini(seq, ctx);
        else
                CDEBUG(D_INFO, "Server Sequence Manager initialized\n");
	return rc;
} 
EXPORT_SYMBOL(seq_server_init);

void
seq_server_fini(struct lu_server_seq *seq,
                const struct lu_context *ctx) 
{
	int rc;

        if (seq->seq_service != NULL) {
                ptlrpc_unregister_service(seq->seq_service);
                seq->seq_service = NULL;
        }

        if (seq->seq_dev != NULL) {
                rc = seq_server_write_state(seq, ctx);
		if (rc) {
			CERROR("can't save sequence state, "
			       "rc = %d\n", rc);
		}
                lu_device_put(&seq->seq_dev->dd_lu_dev);
                seq->seq_dev = NULL;
        }
        
        CDEBUG(D_INFO, "Server Sequence Manager finalized\n");
}
EXPORT_SYMBOL(seq_server_fini);

static int fid_init(void)
{
	ENTRY;
        CDEBUG(D_INFO, "Lustre Sequence Manager\n");
	RETURN(0);
}

static int fid_fini(void)
{
	ENTRY;
	RETURN(0);
}

static int 
__init fid_mod_init(void) 

{
        /* init caches if any */
        fid_init();
        return 0;
}

static void 
__exit fid_mod_exit(void) 
{
        /* free caches if any */
        fid_fini();
        return;
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre FID Module");
MODULE_LICENSE("GPL");

cfs_module(fid, "0.0.3", fid_mod_init, fid_mod_exit);
#endif

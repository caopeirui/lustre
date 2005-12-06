/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *   Copyright (C) 2002 Cluster File Systems, Inc.
 *   Author: Lin Song Tao <lincent@clusterfs.com>
 *   Author: Nathan Rutman <nathan@clusterfs.com>
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
 *
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>

#include <string.h>
#include <getopt.h>

#include <linux/types.h>
#define NO_SYS_VFS 1
#include <linux/fs.h> // for BLKGETSIZE64
#include <linux/lustre_disk.h>
#include <lnet/lnetctl.h>
#include "obdctl.h"

/* So obd.o will link */
#include "parser.h"
command_t cmdlist[] = {
        { 0, 0, 0, NULL }
};

/* FIXME */
#define MAX_LOOP_DEVICES 256

static char *progname;
static int verbose = 1;

/* for running system() */
static char cmd[128];
static char cmd_out[32][128];
static char *ret_file = "/tmp/mkfs.log";
        

void usage(FILE *out)
{
        fprintf(out, "usage: %s [options] <device>\n", progname);

        fprintf(out, 
                "\t<device>:block device or file (e.g /dev/sda or /tmp/ost1)\n"
                "\toptions:\n"
                "\t\t--ost: object storage, mutually exclusive with mdt\n"
                "\t\t--mdt: metadata storage, mutually exclusive with ost\n"
                "\t\t--mgmt: configuration management service - one per site\n"
                "\t\t--mgmtnid=<nid>[,<...>]:nid(s) of a remote mgmt node\n"
                "\t\t--fsname=<filesystem_name>\n"
                "\t\t--configdev=<altdevice|file>: store configuration info\n"
                "\t\t\tfor this device on an alternate device\n"
                "\t\t--failover=<failover-address>\n"
                "\t\t--backfstype=<fstype>: backing fs type (ext3, ldiskfs)\n"
                "\t\t--device_size=#N(KB):device size \n"
                "\t\t--stripe_count=#N:number of stripe\n"
                "\t\t--stripe_size=#N(KB):stripe size\n"
                "\t\t--index=#N:target index\n"
                "\t\t--mountfsoptions=<opts>: permanent mount options\n"
                "\t\t--mkfsoptions=<opts>: format options\n"
                "\t\t--timeout=<secs>: system timeout period\n"
                "\t\t--startupwait=<secs>: time to wait for other servers to join\n"
                "\t\t--reformat: overwrite an existing disk\n"
                "\t\t--verbose\n");
        return;
}

#define vprint if (verbose) printf

static void fatal(void)
{
        verbose = 0;
        fprintf(stderr, "\n%s FATAL: ", progname);
}

/*================ utility functions =====================*/

inline unsigned int 
dev_major (unsigned long long int __dev)
{
        return ((__dev >> 8) & 0xfff) | ((unsigned int) (__dev >> 32) & ~0xfff);
}

inline unsigned int
dev_minor (unsigned long long int __dev)
{
        return (__dev & 0xff) | ((unsigned int) (__dev >> 12) & ~0xff);
}

int get_os_version()
{
        static int version = 0;

        if (!version) {
                int fd;
                char release[4] = "";

                fd = open("/proc/sys/kernel/osrelease", O_RDONLY);
                if (fd < 0) 
                        fprintf(stderr, "Warning: Can't resolve kernel version,"
                        " assuming 2.6\n");
                else {
                        read(fd, release, 4);
                        close(fd);
                }
                if (strncmp(release, "2.4.", 4) == 0) 
                        version = 24;
                else 
                        version = 26;
        }
        return version;
}

//Ugly implement. FIXME 
int run_command(char *cmd)
{
       int i = 0,ret = 0;
       FILE *rfile = NULL;

       vprint("cmd: %s\n", cmd);
       
       strcat(cmd, " >");
       strcat(cmd, ret_file);
       strcat(cmd, " 2>&1");
  
       ret = system(cmd);

       rfile = fopen(ret_file, "r");
       if (rfile == NULL){
                fprintf(stderr,"Could not open %s \n",ret_file);
                exit(2);
       }
      
       memset(cmd_out, 0, sizeof(cmd_out));
       while (fgets(cmd_out[i], 128, rfile) != NULL) {
               if (verbose > 2) printf("  _ %s", cmd_out[i]); 
               i++;
               if (i >= 32) {
                       fprintf(stderr,"WARNING losing some output from %s",
                               cmd);
                       break;
               }
       }
       fclose(rfile);

       return ret;
}

static void run_command_out()
{
        int i;
        for (i = 0; i < 32; i++) {
                if (strlen(cmd_out[i]) == 0)
                        break;
                fprintf(stderr, cmd_out[i]);
        }
}

static int lnet_setup = 0;
static void lnet_start()
{
        ptl_initialize(0, NULL);
        if (access("/proc/sys/lnet", X_OK) != 0) {
                fprintf(stderr, "The LNET module must be loaded to determine "
                        "local NIDs\n");
                exit(1);
        }
        if (jt_ptl_get_nids(NULL) == -ENETDOWN) {
                char *cmd[]={"network", "up"};
                jt_ptl_network(2, cmd);
                lnet_setup++;
        }
}

static void lnet_stop()
{
        char *cmd[]={"network", "down"};
        if (--lnet_setup == 0)
                jt_ptl_network(2, cmd);
}


/*============ disk dev functions ===================*/

/* Setup a file in the first unused loop_device */
int loop_setup(struct mkfs_opts *mop)
{
        char loop_base[20];
        char l_device[64];
        int i,ret = 0;

        /* Figure out the loop device names */
        if (!access("/dev/loop0", F_OK | R_OK))
                strcpy(loop_base, "/dev/loop\0");
        else if (!access("/dev/loop/0", F_OK | R_OK))
                strcpy(loop_base, "/dev/loop/\0");
        else {
                fprintf(stderr, "can't access loop devices\n");
                exit(1);
        }

        /* Find unused loop device */
        for (i = 0; i < MAX_LOOP_DEVICES; i++) {
                sprintf(l_device, "%s%d", loop_base, i);
                if (access(l_device, F_OK | R_OK)) 
                        break;

                sprintf(cmd, "losetup %s", l_device);
                ret = run_command(cmd);
                /* losetup gets 1 (256?) for good non-set-up device */
                if (ret) {
                        /* Setup up a loopback device to our file */
                        sprintf(cmd, "losetup %s %s", l_device, mop->mo_device);
                        ret = run_command(cmd);
                        if (ret) {
                                fprintf(stderr, "error %d on losetup: %s\n",
                                        ret, strerror(ret));
                                exit(8);
                        }
                        strcpy(mop->mo_loopdev, l_device);
                        return ret;
                }
        }
        
        fprintf(stderr,"out of loop devices!\n");
        return EMFILE;
}       

int loop_cleanup(struct mkfs_opts *mop)
{
        int ret = 1;
        if (mop->mo_flags & MO_IS_LOOP) {
                sprintf(cmd, "losetup -d %s", mop->mo_loopdev);
                ret = run_command(cmd);
        }
        return ret;
}

/* Determine if a device is a block device (as opposed to a file) */
int is_block(char* devname)
{
        struct stat st;
        int ret = 0;

        ret = access(devname, F_OK);
        if (ret != 0) 
                return 0;
        ret = stat(devname, &st);
        if (ret != 0) {
                fprintf(stderr, "cannot stat %s\n",devname);
                exit(4);
        }
        return S_ISBLK(st.st_mode);
}

__u64 get_device_size(char* device) 
{
        int ret, fd;
        __u64 size = 0;

#if 0
        sprintf(cmd, "sfdisk -s %s", device);
        ret = run_command(cmd);
        if (ret == 0) {
                size = atoll(cmd_out[0]);
                return (size);
        }
#endif
        /* bz5831 BLKGETSIZE64 */
        fd = open(device, O_RDONLY);
        if (fd < 0) {
                fprintf(stderr, "cannot open %s: %s\n", device, strerror(errno));
                exit(4);
        }

        ret = ioctl(fd, BLKGETSIZE64, (void*)&size);
        close(fd);
        if (ret < 0) {
                fprintf(stderr, "size ioctl failed: %s\n", strerror(errno));
                exit(4);
        }
        
        return size;
}

int loop_format(struct mkfs_opts *mop)
{
        int ret = 0;
       
        if (mop->mo_device_sz == 0) {
                fatal();
                fprintf(stderr, "loop device requires a --device_size= "
                        "param\n");
                return EINVAL;
        }

        ret = creat(mop->mo_device, S_IRUSR|S_IWUSR);
        ret = truncate(mop->mo_device, mop->mo_device_sz * 1024);
        if (ret != 0) {
                ret = errno;
                fprintf(stderr, "Unable to create backing store: %d\n", ret);
        }

        return ret;
}

/* Build fs according to type */
int make_lustre_backfs(struct mkfs_opts *mop)
{
        char mkfs_cmd[256];
        char buf[40];
        char *dev;
        int ret = 0;
        int block_count = 0;

        if (mop->mo_device_sz != 0) {
                if (mop->mo_device_sz < 8096){
                        fprintf(stderr, "size of filesystem must be larger "
                                "than 8MB, but is set to %lldKB\n",
                                mop->mo_device_sz);
                        return EINVAL;
                }
                block_count = mop->mo_device_sz / 4; /* block size is 4096 */
        }       
        
        if ((mop->mo_ldd.ldd_mount_type == LDD_MT_EXT3) ||
            (mop->mo_ldd.ldd_mount_type == LDD_MT_LDISKFS)) { 
                __u64 device_sz = mop->mo_device_sz;

                /* we really need the size */
                if (device_sz == 0)
                        device_sz = get_device_size(mop->mo_device);

                if (strstr(mop->mo_mkfsopts, "-J") == NULL) {
                        /* Choose our own default journal size */
                        long journal_sz = 0;
                        if (device_sz > 1024 * 1024) 
                                journal_sz = (device_sz / 102400) * 4;
                        if (journal_sz > 400)
                                journal_sz = 400;
                        if (journal_sz) {
                                sprintf(buf, " -J size=%ld", journal_sz);
                                strcat(mop->mo_mkfsopts, buf);
                        }
                }

                /* Default bytes_per_inode is block size */
                if (strstr(mop->mo_mkfsopts, "-i") == NULL) {
                        long bytes_per_inode = 0;
                                        
                        if (IS_MDT(&mop->mo_ldd)) 
                                bytes_per_inode = 4096;

                        /* Allocate fewer inodes on large OST devices.  Most
                           filesystems can be much more aggressive than even 
                           this. */
                        if ((IS_OST(&mop->mo_ldd) && (device_sz > 1000000))) 
                                bytes_per_inode = 16384;
                        
                        if (bytes_per_inode > 0) {
                                sprintf(buf, " -i %ld", bytes_per_inode);
                                strcat(mop->mo_mkfsopts, buf);
                        }
                }
                
                /* This is an undocumented mke2fs option. Default is 128. */
                if (strstr(mop->mo_mkfsopts, "-I") == NULL) {
                        long inode_size = 0;
                        if (IS_MDT(&mop->mo_ldd)) {
                                if (mop->mo_stripe_count > 77)
                                        inode_size = 512; /* bz 7241 */
                                else if (mop->mo_stripe_count > 34)
                                        inode_size = 2048;
                                else if (mop->mo_stripe_count > 13)
                                        inode_size = 1024;
                                else 
                                        inode_size = 512;
                        }
                        
                        if (inode_size > 0) {
                                sprintf(buf, " -I %ld", inode_size);
                                strcat(mop->mo_mkfsopts, buf);
                        }
                        
                }

                /* Enable hashed b-tree directory lookup in large dirs bz6224 */
                if (strstr(mop->mo_mkfsopts, "-O") == NULL) {
                        sprintf(buf, " -O dir_index");
                        strcat(mop->mo_mkfsopts, buf);
                }

                sprintf(mkfs_cmd, "mkfs.ext2 -j -b 4096 -L %s ",
                        mop->mo_ldd.ldd_svname);

        } else if (mop->mo_ldd.ldd_mount_type == LDD_MT_REISERFS) {
                long journal_sz = 0; /* FIXME default journal size */
                if (journal_sz > 0) { 
                        sprintf(buf, " --journal_size %ld", journal_sz);
                        strcat(mop->mo_mkfsopts, buf);
                }
                sprintf(mkfs_cmd, "mkreiserfs -ff ");

        } else {
                fprintf(stderr,"unsupported fs type: %d (%s)\n",
                        mop->mo_ldd.ldd_mount_type, 
                        MT_STR(&mop->mo_ldd));
                return EINVAL;
        }

        /* Loop device? */
        dev = mop->mo_device;
        if (mop->mo_flags & MO_IS_LOOP) 
                dev = mop->mo_loopdev;
        
        vprint("formatting backing filesystem %s on %s\n",
               MT_STR(&mop->mo_ldd), dev);
        vprint("\tservice name  %s\n", mop->mo_ldd.ldd_svname);
        vprint("\t4k blocks     %d\n", block_count);
        vprint("\toptions       %s\n", mop->mo_mkfsopts);

        /* mkfs_cmd's trailing space is important! */
        strcat(mkfs_cmd, mop->mo_mkfsopts);
        strcat(mkfs_cmd, " ");
        strcat(mkfs_cmd, dev);
        if (block_count != 0) {
                sprintf(buf, " %d", block_count);
                strcat(mkfs_cmd, buf);
        }

        vprint("mkfs_cmd = %s\n", mkfs_cmd);
        ret = run_command(mkfs_cmd);
        if (ret != 0) {
                fatal();
                fprintf(stderr, "Unable to build fs: %s \n", dev);
                run_command_out();
                goto out;
        }

out:
        return ret;
}

/* ==================== Lustre config functions =============*/

void print_ldd(struct lustre_disk_data *ldd)
{
        int i = 0;
        printf("\nPermanent disk data:\n");
        printf("Server:     %s\n", ldd->ldd_svname);
        printf("Lustre FS:  %s\n", ldd->ldd_fsname);
        printf("Mount type: %s\n", MT_STR(ldd));
        printf("Flags:      %s%s%s%s\n",
               IS_MDT(ldd) ? "MDT ":"", IS_OST(ldd) ? "OST ":"",
               IS_MGMT(ldd) ? "MGMT ":"",
               ldd->ldd_flags & LDD_F_NEED_INDEX   ? "needs_index ":"");
        printf("Persistent mount opts: %s\n", ldd->ldd_mount_opts);
        printf("MGS nids: ");
        for (i = 0; i < ldd->ldd_mgsnid_count; i++) {
                printf("%c %s", (i == 0) ? ' ' : ',',
                       libcfs_nid2str(ldd->ldd_mgsnid[i]));
        }
        printf("\n\n");
}

/* Write the server config files */
int write_local_files(struct mkfs_opts *mop)
{
        struct lr_server_data lsd;
        char mntpt[] = "/tmp/mntXXXXXX";
        char filepnm[128];
        char *dev;
        FILE *filep;
        int ret = 0;

        /* Mount this device temporarily in order to write these files */
        vprint("mounting backing device\n");
        if (!mkdtemp(mntpt)) {
                fprintf(stderr, "Can't create temp mount point %s: %s\n",
                        mntpt, strerror(errno));
                return errno;
        }

        dev = mop->mo_device;
        if (mop->mo_flags & MO_IS_LOOP) 
                dev = mop->mo_loopdev;
        
        ret = mount(dev, mntpt, MT_STR(&mop->mo_ldd), 0, NULL);
        if (ret) {
                fprintf(stderr, "Unable to mount %s: %s\n", mop->mo_device,
                        strerror(ret));
                goto out_rmdir;
        }

        /* Set up initial directories */
        sprintf(filepnm, "%s/%s", mntpt, MOUNT_CONFIGS_DIR);
        ret = mkdir(filepnm, 0777);
        if (ret) {
                fprintf(stderr, "Can't make configs dir %s (%d)\n", 
                        filepnm, ret);
                goto out_umnt;
        }

        /* Save the persistent mount data into a file. Lustre must pre-read
           this file to get the real mount options. */
        vprint("Writing %s\n", MOUNT_DATA_FILE);
        sprintf(filepnm, "%s/%s", mntpt, MOUNT_DATA_FILE);
        filep = fopen(filepnm, "w");
        if (!filep) {
                fprintf(stderr, "Unable to create %s file\n", filepnm);
                goto out_umnt;
        }
        fwrite(&mop->mo_ldd, sizeof(mop->mo_ldd), 1, filep);
        fclose(filep);
        
        /* Create the inital last_rcvd file */
        vprint("Writing %s\n", LAST_RCVD);
        sprintf(filepnm, "%s/%s", mntpt, LAST_RCVD);
        filep = fopen(filepnm, "w");
        if (!filep) {
                ret = errno;
                fprintf(stderr,"Unable to create %s file\n", filepnm);
                goto out_umnt;
        }
        memset(&lsd, 0, sizeof(lsd));
        strncpy(lsd.lsd_uuid, mop->mo_ldd.ldd_svname, sizeof(lsd.lsd_uuid));
        // FIXME any need for the lsd_index? 
        lsd.lsd_index = mop->mo_ldd.ldd_svindex;
        lsd.lsd_feature_compat |= cpu_to_le32(LR_COMPAT_COMMON_LR);
        lsd.lsd_server_size = cpu_to_le32(LR_SERVER_SIZE);
        lsd.lsd_client_start = cpu_to_le32(LR_CLIENT_START);
        lsd.lsd_client_size = cpu_to_le16(LR_CLIENT_SIZE);
        if (IS_MDT(&mop->mo_ldd))
                lsd.lsd_feature_rocompat = cpu_to_le32(MDS_ROCOMPAT_LOVOBJID);
        
        fwrite(&lsd, sizeof(lsd), 1, filep);
        ret = 0;
        fclose(filep);
out_umnt:
        vprint("unmounting backing device\n");
        umount(mntpt);    
out_rmdir:
        rmdir(mntpt);
        return ret;
}

void set_defaults(struct mkfs_opts *mop)
{
        mop->mo_ldd.ldd_magic = LDD_MAGIC;
        mop->mo_ldd.ldd_config_ver = 0;
        mop->mo_ldd.ldd_flags = LDD_F_NEED_INDEX | LDD_F_NEED_REGISTER;
        mop->mo_ldd.ldd_mgsnid_count = 0;
        strcpy(mop->mo_ldd.ldd_fsname, "lustre");
        if (get_os_version() == 24) 
                mop->mo_ldd.ldd_mount_type = LDD_MT_EXT3;
        else 
                mop->mo_ldd.ldd_mount_type = LDD_MT_LDISKFS;
        
        mop->mo_ldd.ldd_svindex = -1;
        mop->mo_stripe_count = 1;
}

static inline void badopt(char opt, char *type)
{
        fprintf(stderr, "%s: '%c' only valid for %s\n",
                progname, opt, type);
        usage(stderr);
        exit(1);
}

int main(int argc , char *const argv[])
{
        struct mkfs_opts mop;
        static struct option long_opt[] = {
                {"backfstype", 1, 0, 'b'},
                {"configdev", 1, 0, 'C'},
                {"device_size", 1, 0, 'd'},
                {"fsname",1, 0, 'n'},
                {"failover", 1, 0, 'f'},
                {"help", 0, 0, 'h'},
                {"mdt", 0, 0, 'M'},
                {"mgmt", 0, 0, 'G'},
                {"mgmtnid", 1, 0, 'm'},
                {"mkfsoptions", 1, 0, 'k'},
                {"mountfsoptions", 1, 0, 'o'},
                {"ost", 0, 0, 'O'},
                {"reformat", 0, 0, 'r'},
                {"startupwait", 1, 0, 'w'},
                {"stripe_count", 1, 0, 'c'},
                {"stripe_size", 1, 0, 's'},
                {"stripe_index", 1, 0, 'i'},
                {"index", 1, 0, 'i'},
                {"timeout", 1, 0, 't'},
                {"verbose", 0, 0, 'v'},
                {0, 0, 0, 0}
        };
        char *optstring = "b:C:d:n:f:hI:MGm:k:o:Orw:c:s:i:t:v";
        char opt;
        char *mountopts = NULL;
        int  ret = 0;

        progname = argv[0];
        if (argc < 3) {
                usage(stderr);
                exit(0);
        }

        memset(&mop, 0, sizeof(mop));
        set_defaults(&mop);

        while ((opt = getopt_long(argc, argv, optstring, long_opt, NULL)) != 
               EOF) {
                switch (opt) {
                case 'b': {
                        int i = 0;
                        while (i < LDD_MT_LAST) {
                                if (strcmp(optarg, mt_str(i)) == 0) {
                                        mop.mo_ldd.ldd_mount_type = i;
                                        break;
                                }
                                i++;
                        }
                        break;
                }
                case 'C': /* Configdev */
                        //FIXME
                        exit(2);
                case 'c':
                        if (IS_MDT(&mop.mo_ldd)) {
                                int stripe_count = atol(optarg);
                                mop.mo_stripe_count = stripe_count;
                        } else {
                                badopt(opt, "MDT");
                        }
                        break;
                case 'd':
                        mop.mo_device_sz = atol(optarg); 
                        break;
                case 'k':
                        strncpy(mop.mo_mkfsopts, optarg, 
                                sizeof(mop.mo_mkfsopts) - 1);
                        break;
                case 'f':
                        /* we must pass this info on when we register with
                           the mgs */
                        //mop.mo_hostnid.backup = libcfs_str2nid(optarg);
                        break;
                case 'G':
                        mop.mo_ldd.ldd_flags |= LDD_F_SV_TYPE_MGMT;
                        break;
                case 'h':
                        usage(stdout);
                        exit(0);
                case 'i':
                        if (IS_MDT(&mop.mo_ldd) || IS_OST(&mop.mo_ldd)) {
                                mop.mo_ldd.ldd_svindex = atoi(optarg);
                                mop.mo_ldd.ldd_flags &= ~LDD_F_NEED_INDEX;
                        } else {
                                badopt(opt, "MDT,OST");
                        }
                        break;
                case 'm': {
                        int i = 0;
                        char *s1 = optarg, *s2;
                        if (IS_MGMT(&mop.mo_ldd))
                                badopt(opt, "non-MGMT MDT,OST");
                        while ((s2 = strsep(&s1, ","))) {
                                mop.mo_ldd.ldd_mgsnid[i++] =
                                        libcfs_str2nid(s2);
                                if (i >= MAX_FAILOVER_NIDS) {
                                        fprintf(stderr, "too many MGS nids, "
                                                "ignoring %s\n", s1);
                                        break;
                                }
                        }
                        mop.mo_ldd.ldd_mgsnid_count = i;
                        break;
                }
                case 'M':
                        mop.mo_ldd.ldd_flags |= LDD_F_SV_TYPE_MDT;
                        break;
                case 'n':
                        if (!(IS_MDT(&mop.mo_ldd) || IS_OST(&mop.mo_ldd)))
                                badopt(opt, "MDT,OST");
                        if (strlen(optarg) > 8) {
                                fprintf(stderr, "%s: filesystem name must be "
                                        "<= 8 chars\n", progname);
                                exit(1);
                        }
                        if (optarg[0] != 0) 
                                strncpy(mop.mo_ldd.ldd_fsname, optarg, 
                                        sizeof(mop.mo_ldd.ldd_fsname) - 1);
                        break;
                case 'o':
                        mountopts = optarg;
                        break;
                case 'O':
                        mop.mo_ldd.ldd_flags |= LDD_F_SV_TYPE_OST;
                        break;
                case 'r':
                        mop.mo_flags |= MO_FORCEFORMAT;
                        break;
                case 's':
                        if (IS_MDT(&mop.mo_ldd)) 
                                mop.mo_stripe_sz = atol(optarg) * 1024;
                        else 
                                badopt(opt, "MDT");
                        break;
                case 't':
                        mop.mo_timeout = atol(optarg);
                        break;
                case 'v':
                        verbose++;
                        break;
                default:
                        if (opt != '?') {
                                fatal();
                                fprintf(stderr, "Unknown option '%c'\n", opt);
                        }
                        usage(stderr);
                        exit(1);
                }
        }//while
        if (optind >= argc) {
                fatal();
                fprintf(stderr, "Bad arguments\n");
                usage(stderr);
                exit(1);
        }

        if (!(IS_MDT(&mop.mo_ldd) || IS_OST(&mop.mo_ldd) || 
              IS_MGMT(&mop.mo_ldd))) {
                fatal();
                fprintf(stderr, "must set server type :{mdt,ost,mgmt}\n");
                usage(stderr);
                exit(1);
        }

        if (IS_MDT(&mop.mo_ldd) && !IS_MGMT(&mop.mo_ldd) && 
            mop.mo_ldd.ldd_mgsnid_count == 0) {
                vprint("No management node specified, adding MGS to this MDT\n");
                mop.mo_ldd.ldd_flags |= LDD_F_SV_TYPE_MGMT;
        }

        if (IS_MGMT(&mop.mo_ldd) && (mop.mo_ldd.ldd_mgsnid_count == 0)) {
                int i;
                __u64 *nids;
                
                vprint("No mgmt nids specified, using all local nids\n");
                lnet_start();
                i = jt_ptl_get_nids(&nids);
                if (i < 0) {
                        fprintf(stderr, "Can't find local nids "
                                "(is the lnet module loaded?)\n");
                } else {
                        if (i > 0) {
                                if (i > MAX_FAILOVER_NIDS) 
                                        i = MAX_FAILOVER_NIDS;
                                vprint("Adding %d local nids for MGS\n", i);
                                memcpy(mop.mo_ldd.ldd_mgsnid, nids,
                                       sizeof(mop.mo_ldd.ldd_mgsnid));
                                free(nids);
                        }
                        mop.mo_ldd.ldd_mgsnid_count = i;
                }
        }

        if (mop.mo_ldd.ldd_mgsnid_count == 0) {
                fatal();
                fprintf(stderr, "Must specify either --mgmt or --mgmtnode\n");
                usage(stderr);
                goto out;
        }

        if (IS_MDT(&mop.mo_ldd) && (mop.mo_stripe_sz == 0))
                mop.mo_stripe_sz = 1024 * 1024;
        
        strcpy(mop.mo_device, argv[optind]);
        
        /* These are the permanent mount options (always included) */ 
        switch (mop.mo_ldd.ldd_mount_type) {
        case LDD_MT_EXT3:
        case LDD_MT_LDISKFS: {
                sprintf(mop.mo_ldd.ldd_mount_opts, "errors=remount-ro");
                if (IS_MDT(&mop.mo_ldd))
                        strcat(mop.mo_ldd.ldd_mount_opts,
                               ",iopen_nopriv,user_xattr");
                if ((get_os_version() == 24) && IS_OST(&mop.mo_ldd))
                        strcat(mop.mo_ldd.ldd_mount_opts, ",asyncdel");
#if 0
                /* Files created while extents are enabled cannot be read if
                   mounted with a kernel that doesn't include the CFS patches.*/
                if ((get_os_version() == 26) && IS_OST(&mop.mo_ldd) && 
                    mop.mo_ldd.ldd_mount_type == LDD_MT_LDISKFS) {
                        strcat(mop.mo_ldd.ldd_mount_opts, ",extents,mballoc");
                }
#endif               
 
                break;
        }
        case LDD_MT_SMFS: {
                sprintf(mop.mo_ldd.ldd_mount_opts, "type=ext3,dev=%s",
                        mop.mo_device);
                break;
        }
        default: {
                fatal();
                fprintf(stderr, "%s: unknown fs type %d '%s'\n",
                        progname, mop.mo_ldd.ldd_mount_type,
                        MT_STR(&mop.mo_ldd));
                ret = EINVAL;
                goto out;
        }
        }               

        /* User supplied */
        if (mountopts) {
                strcat(mop.mo_ldd.ldd_mount_opts, ",");
                strcat(mop.mo_ldd.ldd_mount_opts, mountopts);
        }

        /* Are we using a loop device? */
        if (!is_block(mop.mo_device) || 
            (mop.mo_ldd.ldd_mount_type == LDD_MT_SMFS))
                mop.mo_flags |= MO_IS_LOOP;
                
        ldd_make_sv_name(&(mop.mo_ldd));

        /* Create the loopback file */
        if (mop.mo_flags & MO_IS_LOOP) {
                ret = loop_format(&mop);
                if (!ret)
                        ret = loop_setup(&mop);
                if (ret) {
                        fatal();
                        fprintf(stderr, "Loop device setup failed: %s\n", 
                                strerror(ret));
                        goto out;
                }
        }

        if (verbose)
                print_ldd(&(mop.mo_ldd));

        ret = make_lustre_backfs(&mop);
        if (ret != 0) {
                fatal();
                fprintf(stderr, "mkfs failed %d\n", ret);
                goto out;
        }
        
        ret = write_local_files(&mop);
        if (ret != 0) {
                fatal();
                fprintf(stderr, "failed to write local files\n");
                goto out;
        }

        /* We will not write startup logs here.  That is the domain of the 
           mgc/mgs, and should probably be done at first mount. 
           mgc might have to pass info from the mount_data_file to mgs. */
#if 0
        ret = write_llog_files(&mop);
        if (ret != 0) {
                fatal();
                fprintf(stderr, "failed to write setup logs\n");
                goto out:
        }
#endif
             
out:
        loop_cleanup(&mop);      
        lnet_stop();

        return ret;
}

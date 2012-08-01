/**
 * \file pierrefs.c
 * \brief Exported functions by the PierreFS file system
 * \author Pierre Schweitzer <pierre.jean.schweitzer@cern.ch>
 * \version 1.0
 * \date 10-Dec-2011
 * \copyright GNU General Public License - GPL
 * \todo Disallow .me. and .wh. files creation
 * \todo Identical files on RO/RW after mod
 */

#include "pierrefs.h"

static int pierrefs_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *kstbuf) {
	int err;
	char *path = get_context()->global1;

	/* Get path */
	err = get_relative_path(0, dentry, path, 1);
	if (err < 0) {
		return err;
	}

	/* Call worker */
	return get_file_attr(path, kstbuf);
}

static int pierrefs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry) {
	int err, origin;
	char *from = get_context()->global1;
	char *to = get_context()->global2;
	char real_from[PATH_MAX];
	char real_to[PATH_MAX];

	/* First, find file */
	err = get_relative_path(0, old_dentry, from, 1);
	if (err < 0) {
		return err;
	}

	origin = find_file(from, real_from, 0);
	if (origin < 0) {
		return origin;
	}

	/* Find destination */
	err = get_relative_path_for_file(dir, dentry, to, 1);
	if (err < 0) {
		return err;
	}

	/* And ensure it doesn't exist */
	err = find_file(to, real_to, 0);
	if (err >= 0) {
		return -EEXIST;
	}

	/* Check access */
	err = can_create(to, real_to);
	if (err < 0) {
		return err;
	}

	/* Create path if needed */
	err = find_path(to, real_to);
	if (err < 0) {
		return err;
	}

	if (origin == READ_ONLY) {
		/* Here, fallback to a symlink */
		err = symlink_worker(real_from, real_to);
		if (err < 0) {
			return err;
		}
	}
	else {
		/* Get RW name */
		if (make_rw_path(to, real_to) > PATH_MAX) {
			return -ENAMETOOLONG;
		}

		err = link_worker(real_from, real_to);
		if (err < 0) {
			return err;
		}
	}

	/* Remove possible whiteout */
	unlink_whiteout(to);

	return 0;
}

static loff_t pierrefs_llseek(struct file *file, loff_t offset, int origin) {
	int err = -EINVAL;
	struct file *real_file = (struct file *)file->private_data;

	if (real_file->f_op->llseek) {
		err = real_file->f_op->llseek(real_file, offset, origin);
	}

	return err;
}

static struct dentry * pierrefs_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nameidata) {
	/* We are looking for "dentry" in "dir" */
	int err;
	char *path = get_context()->global1;
	char *real_path = get_context()->global2;
	struct inode *inode = NULL;

	/* First get path of the file */
	err = get_relative_path_for_file(dir, dentry, path, 1);
	if (err < 0) {
		return ERR_PTR(err);
	}

	/* Now, look for the file */
	err = find_file(path, real_path, 0);
	if (err < 0) {
		return ERR_PTR(err);
	}

	/* We've got it!
	 * Set dentry operations
	 */
	dentry->d_op = &pierrefs_dops;
	/* Set our inode */
	d_add(dentry, inode);

	return NULL;
}

static int pierrefs_mkdir(struct inode *dir, struct dentry *dentry, int mode) {
	int err;
	char *path = get_context()->global1;
	char *real_path = get_context()->global2;

	/* Try to find the directory first */
	err = get_relative_path_for_file(dir, dentry, path, 1);
	if (err < 0) {
		return err;
	}

	/* And ensure it doesn't exist */
	err = find_file(path, real_path, 0);
	if (err >= 0) {
		return -EEXIST;
	}

	/* Get full path for destination */
	if (make_rw_path(path, real_path) > PATH_MAX) {
		return -ENAMETOOLONG;
	}

	/* Check access */
	err = can_create(path, real_path);
	if (err < 0) {
		return err;
	}

	/* Now, create/reuse arborescence */
	err = find_path(path, real_path);
	if (err < 0) {
		return err;
	}

	/* Ensure we have good mode */
	mode |= S_IFDIR;

	/* Just create dir now */
	err = mkdir_worker(real_path, mode);
	if (err < 0) {
		return err;
	}

	/* Hide contents */
	err = hide_directory_contents(path);
	if (err < 0) {
		dentry = get_path_dentry(real_path, LOOKUP_REVAL);
		if (IS_ERR(dentry)) {
			return err;
		}

		push_root();
		vfs_unlink(dentry->d_inode, dentry);
		pop_root();
		dput(dentry);

		return err;
	}

	/* Remove possible .wh. */
	unlink_whiteout(path);

	return 0;
}

static int pierrefs_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t rdev) {
	int err;
	char *path = get_context()->global1;
	char *real_path = get_context()->global2;

	/* Try to find the node first */
	err = get_relative_path_for_file(dir, dentry, path, 1);
	if (err < 0) {
		return err;
	}

	/* And ensure it doesn't exist */
	err = find_file(path, real_path, 0);
	if (err >= 0) {
		return -EEXIST;
	}

	/* Now, create/reuse arborescence */
	err = find_path(path, real_path);
	if (err < 0) {
		return err;
	}

	/* Just create file now */
	if (S_ISFIFO(mode)) {
		err = mkfifo(real_path, mode);
		if (err < 0) {
			return err;
		}
	}
	else {
		err = mknod(real_path, mode, rdev);
		if (err < 0) {
			return err;
		}
	}

	/* Remove possible whiteout */
	unlink_whiteout(path);

	return 0;
}

static int pierrefs_open(struct inode *inode, struct file *file) {
	int err;
	char *path = get_context()->global1;
	char *real_path = get_context()->global2;

	/* Don't check for flags here, if we are down here
	 * the user is allowed to read/write the file, the
	 * file was created if required (and allowed).
	 * Here, the only operation required is to open the
	 * file on the underlying file system
	 */

	/* Get our file path */
	err = get_relative_path(inode, file->f_dentry, path, 1);

	/* Get real file path */
	err = find_file(path, real_path, 0);
	if (err < 0) {
		return err;
	}

	/* Really open the file.
	 * The associated file object on real file system is stored
	 * as private data of the PierreFS file object. This is used
	 * to maintain data consistency and to forward requests on
	 * the file to the lower file system.
	 */
	file->private_data = open_worker_2(real_path, file->f_flags, file->f_mode);
	if (IS_ERR(file->private_data)) {
		err = PTR_ERR(file->private_data);
		file->private_data = 0;
		return err;
	}

	return 0;
}

static int pierrefs_permission(struct inode *inode, int mask, struct nameidata *nd) {
	int err;
	char *path = get_context()->global1;
	char *real_path = get_context()->global2;

	/* Get path */
	err = get_relative_path(0, nd->dentry, path, 1);
	if (err) {
		return err;
	}

	/* Get file */
	err = find_file(path, real_path, 0);
	if (err < 0) {
		return err;
	}

	/* And call worker */
	return can_access(path, real_path, mask);
}

static int pierrefs_setattr(struct dentry *dentry, struct iattr *attr) {
	int err;
	char *path = get_context()->global1;
	char *real_path = get_context()->global2;

	/* Get path */
	err = get_relative_path(0, dentry, path, 1);
	if (err) {
		return err;
	}

	/* Get file */
	err = find_file(path, real_path, 0);
	if (err < 0) {
		return err;
	}

	if (err == READ_WRITE || err == READ_WRITE_COPYUP) {
		/* Just update file attributes */
		return notify_change(dentry, attr);
    }

	/* Update me
	 * Don't clear flags, set_me_worker will do
	 * So, only call the worker
	 */
	return set_me_worker(path, real_path, attr);
}

static int pierrefs_symlink(struct inode *dir, struct dentry *dentry, const char *symname) {
	/* Create the link on the RW branch */
	int err;
	char *to = get_context()->global1;
	char *real_to = get_context()->global2;

	/* Find destination */
	err = get_relative_path_for_file(dir, dentry, to, 1);
	if (err < 0) {
		return err;
	}

	/* And ensure it doesn't exist */
	err = find_file(to, real_to, 0);
	if (err >= 0) {
		return -EEXIST;
	}

	/* Get full path for destination */
	if (make_rw_path(to, real_to) > PATH_MAX) {
		return -ENAMETOOLONG;
	}

	/* Check access */
	err = can_create(to, real_to);
	if (err < 0) {
		return err;
	}

	/* Create path if needed */
	err = find_path(to, real_to);
	if (err < 0) {
		return err;
	}

	/* Now it's sure the link does not exist, create it */
	err = symlink_worker(symname, real_to);
	if (err < 0) {
		return err;
	}

	/* Remove possible whiteout */
	unlink_whiteout(to);

	return 0;
}

static int pierrefs_statfs(struct dentry *dentry, struct kstatfs *buf) {
	struct super_block *sb = dentry->d_sb;
	struct pierrefs_sb_info * sb_info = sb->s_fs_info;
	struct file *filp;
	int err;

	memset(buf, 0, sizeof(*buf));

	/* First, get RO data */
	filp = filp_open(sb_info->read_only_branch, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		pr_err("Failed opening RO branch!\n");
		return PTR_ERR(filp);
	}

	err = vfs_statfs(filp->f_dentry, buf);
	filp_close(filp, 0);

	if (unlikely(err)) {
		return err;
	}

	/* Return them, but ensure we mark our stuff */
	buf->f_type = sb->s_magic;
	memset(&buf->f_fsid, 0, sizeof(buf->f_fsid));

	return 0;
}

struct inode_operations pierrefs_iops = {
	.getattr	= pierrefs_getattr,
	.link		= pierrefs_link,
	.lookup		= pierrefs_lookup,
	.mkdir		= pierrefs_mkdir,
	.mknod		= pierrefs_mknod,
	.permission	= pierrefs_permission,
#if 0
	.readlink	= generic_readlink, /* dentry will already point on the right file */
#endif
	.setattr	= pierrefs_setattr,
	.symlink	= pierrefs_symlink,
};

struct super_operations pierrefs_sops = {
	.statfs		= pierrefs_statfs,
};

struct dentry_operations pierrefs_dops = {
};

struct file_operations pierrefs_fops = {
	.llseek		= pierrefs_llseek,
	.open		= pierrefs_open,
};

struct file_operations pierrefs_drops = {
};

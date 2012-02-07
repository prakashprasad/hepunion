/**
 * \file main.c
 * \brief Entry point of the PierreFS file system
 * \author Pierre Schweitzer <pierre.jean.schweitzer@cern.ch>
 * \version 1.0
 * \date 21-Nov-2011
 * \copyright GNU General Public License - GPL
 *
 * This is where arguments of the command line will be handle.
 * This includes branches discovery.
 * It fills in mount context in case of success.
 */

#include "pierrefs.h"

MODULE_AUTHOR("Pierre Schweitzer, CERN CH"
	      " (http://pierrefs.sourceforge.net)");
MODULE_DESCRIPTION("PierreFS " PIERREFS_VERSION
		   " (http://pierrefs.sourceforge.net)");
MODULE_LICENSE("GPL"); 

static int make_path(const char *s, size_t n, char **path) {
    /* Zero output */
    *path = 0;

	/* First of all, look if it is relative path */
	if (s[0] != '/') {
        return -EINVAL;
	}

	/* Tailing has to be removed */
	if (s[n - 1] == '/') {
		n--;
	}

	/* Allocate one more ('\0') */
	*path = kmalloc((n + 1) * sizeof(char), GFP_NOFS);
	if (*path) {
		memcpy(*path, s, n);
		*path[n] = '\0';
        return 0;
	}

    return -ENOMEM;
}

static int get_branches(struct super_block *sb, const char *arg) {
	int err, forced_ro = 0;
	char *output, *type, *part2;
	struct pierrefs_sb_info * sb_info = sb->s_fs_info;
	struct inode * root_i;
	umode_t root_m;
	struct timespec atime, mtime, ctime;
	struct file *filp;

	/* We are expecting 2 branches, separated by : */
	part2 = strchr(arg, ':');
	if (!part2) {
		return -EINVAL;
	}

	/* Look for first branch type */
	type = strchr(arg, '=');
	/* First branch has a type */
	if (type && type < part2) {
		/* Get branch name */
		err = make_path(arg, type - arg, &output);
		if (err || !output) {
			return err;
		}

		if (!strncmp(type + 1, "RW", 2)) {
			sb_info->read_write_branch = output;
		}
		else if (strncmp(type + 1, "RO", 2)) {
			return -EINVAL;
		}
		else {
			sb_info->read_only_branch = output;
			forced_ro = 1;
		}

		/* Get type for second branch */
		type = strchr(part2, '=');
	}
	/* It has no type => RO */
	else {
		/* Get branch name */
		err = make_path(arg, part2 - arg, &sb_info->read_only_branch);
		if (err || !sb_info->read_only_branch) {
			return err;
		}
	}

	/* Skip : */
	part2++;

	/* If second branch has a type */
	if (type) {
		/* Get branch name */
		err = make_path(part2, type - part2, &output);
		if (err || !output) {
			return err;
		}

		if (!strncmp(type + 1, "RW", 2)) {
			if (sb_info->read_write_branch) {
				return -EINVAL;
			}
			sb_info->read_write_branch = output;
		}
		else if (strncmp(type + 1, "RO", 2)) {
			return -EINVAL;
		}
		else {
			if (forced_ro) {
				return -EINVAL;
			}
			sb_info->read_only_branch = output;
		}
	}
	else {
		/* It has no type, adapt given the situation */
		if (sb_info->read_write_branch) {
			err = make_path(part2, strlen(part2), &sb_info->read_only_branch);
			if (err || !sb_info->read_only_branch) {
				return err;
			}
		}
		else if (sb_info->read_only_branch) {
			err = make_path(part2, strlen(part2), &sb_info->read_write_branch);
			if (err || !sb_info->read_write_branch) {
				return err;
			}
		}
	}

	/* At this point, we should have the two branches set */
	if (!sb_info->read_only_branch || !sb_info->read_write_branch) {
		return -EINVAL;
	}

	/* Check for branches */
	filp = filp_open(sb_info->read_only_branch, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		return PTR_ERR(filp);
	}

	/* Get superblock data from RO branch and set to ours */
	sb->s_blocksize = filp->f_vfsmnt->mnt_sb->s_blocksize;
	sb->s_blocksize_bits = filp->f_vfsmnt->mnt_sb->s_blocksize_bits;
	/* Root modes - FIXME (check for me & merge) */
	root_m = filp->f_vfsmnt->mnt_sb->s_root->d_inode->i_mode;
	atime = filp->f_vfsmnt->mnt_sb->s_root->d_inode->i_atime;
	mtime = filp->f_vfsmnt->mnt_sb->s_root->d_inode->i_mtime;
	ctime = filp->f_vfsmnt->mnt_sb->s_root->d_inode->i_ctime;

	/* Finally close */
	filp_close(filp, 0);

	/* Check for consistent data */
	if (!is_flag_set(root_m, S_IFDIR)) {
		return -EINVAL;
	}

	filp = filp_open(sb_info->read_write_branch, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		return PTR_ERR(filp);
	}
	filp_close(filp, 0);

	/* Allocate inode for / */
	root_i = new_inode(sb);
	if (IS_ERR(root_i)) {
		return PTR_ERR(root_i);
	}

	/* Init it */
	root_i->i_ino = 1;
	root_i->i_mode = root_m;
	root_i->i_atime = atime;
	root_i->i_mtime = mtime;
	root_i->i_ctime = ctime;
	root_i->i_op = &pierrefs_iops;
	root_i->i_nlink = 2;

	/* Create its directory entry */
	sb->s_root = d_alloc_root(root_i);
	if (IS_ERR(sb->s_root)) {
		clear_inode(root_i);
		return PTR_ERR(sb->s_root);
	}
	sb->s_root->d_op = &pierrefs_dops;

	/* Set super block attributes */
	sb->s_magic = PIERREFS_MAGIC;
	sb->s_op = &pierrefs_sops;
	sb->s_time_gran = 1;

	/* TODO: Add directory entries */

	return 0;
}

static int pierrefs_read_super(struct super_block *sb, void *raw_data,
			       int silent) {
	int err;
	struct pierrefs_sb_info *sb_info;

	/* Check for parameters */
	if (!raw_data) {
		return -EINVAL;
	}

	/* Allocate super block info structure */
	sb_info =
	sb->s_fs_info = kzalloc(sizeof(struct pierrefs_sb_info), GFP_KERNEL);
	if (unlikely(!sb->s_fs_info)) {
		return -ENOMEM;
	}

	/* Get branches */
	err = get_branches(sb, raw_data);
	if (err) {
		if (sb_info->read_only_branch) {
			kfree(sb_info->read_only_branch);
		}
		if (sb_info->read_write_branch) {
			kfree(sb_info->read_write_branch);
		}
		kfree(sb_info);
		sb->s_fs_info = NULL;
		return err;
	}

	return 0;
}

static int pierrefs_get_sb(struct file_system_type *fs_type,
				     int flags, const char *dev_name,
				     void *raw_data, struct vfsmount *mnt) {
	int err = get_sb_nodev(fs_type, flags,
					    raw_data, pierrefs_read_super, mnt);

	return err;
}

static void pierrefs_kill_sb(struct super_block *sb) {
	struct pierrefs_sb_info *sb_info;

	sb_info = sb->s_fs_info;

	if (sb_info->read_only_branch) {
		kfree(sb_info->read_only_branch);
	}
	if (sb_info->read_write_branch) {
		kfree(sb_info->read_write_branch);
	}

	kill_litter_super(sb);
}

static struct file_system_type pierrefs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= PIERREFS_NAME,
	.get_sb		= pierrefs_get_sb,
	.kill_sb	= pierrefs_kill_sb,
	.fs_flags	= FS_REVAL_DOT,
};

static int __init init_pierrefs_fs(void) {
	return register_filesystem(&pierrefs_fs_type);
}

static void __exit exit_pierrefs_fs(void) {
	unregister_filesystem(&pierrefs_fs_type);
}

module_init(init_pierrefs_fs);
module_exit(exit_pierrefs_fs);
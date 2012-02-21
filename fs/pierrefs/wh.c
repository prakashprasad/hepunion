/**
 * \file wh.c
 * \brief Whiteout (WH) support for the PierreFS file system
 * \author Pierre Schweitzer <pierre.jean.schweitzer@cern.ch>
 * \version 1.0
 * \date 11-Jan-2012
 * \copyright GNU General Public License - GPL
 *
 * Whiteout is the mechanism that allows file and directory
 * deletion on the read-only branch.
 *
 * When a demand to delete a file on the read-only branch is
 * made, the PierreFS file system will a matching whiteout file
 * on the read-write branch.
 *
 * That way, during union, whiteout files will be used to hide
 * some files from the read-only branch.
 *
 * Deleting the whiteout "recovers" the file.
 *
 * Whiteouts consist in files called .wh.{original file}
 *
 * This is based on the great work done by the UnionFS driver
 * team.
 */

#include "pierrefs.h"

static int create_whiteout_worker(const char *wh_path) {
	int err;
	struct iattr attr;
	struct dentry *dentry;

	/* Create file */
	struct file *fd = creat_worker(wh_path, S_IRUSR);
	if (IS_ERR(fd)) {
		return PTR_ERR(fd);
	}

	/* Set owner to root */
	attr.ia_valid = ATTR_UID | ATTR_GID;
	attr.ia_gid = 0;
	attr.ia_uid = 0;

	err = notify_change(fd->f_dentry->d_inode, &attr);
	if (err == 0) {
		return err;
	}

	/* Save dentry */
	dentry = fd->f_dentry;
	dget(dentry);

	/* Close file and delete it */
	filp_close(fd, 0);
	vfs_unlink(fd->f_dentry->d_inode, fd->f_dentry);

	dput(dentry);

	return err;
}

int create_whiteout(const char *path, char *wh_path) {
	int err;

	/* Find name */
	char *tree_path = strrchr(path, '/');
	if (!tree_path) {
		return -EINVAL;
	}

	if (snprintf(wh_path, PATH_MAX, "%s", get_context()->read_write_branch) > PATH_MAX) {
		return -ENAMETOOLONG;
	}

	/* Copy path (and /) */
	strncat(wh_path, path, tree_path - path + 1);
	/* Append wh */
	strcat(wh_path, ".wh.");
	/* Finalement copy name */
	strcat(wh_path, tree_path + 1);

	/* Ensure path exists */
	err = find_path(path, NULL);
	if (err < 0) {
		return err;
	}

	/* Call worker */
	return create_whiteout_worker(wh_path);
}

int find_whiteout(const char *path, char *wh_path) {
	struct kstat kstbuf;

	/* Find name */
	char *tree_path = strrchr(path, '/');
	if (!tree_path) {
		return -EINVAL;
	}

	if (snprintf(wh_path, PATH_MAX, "%s", get_context()->read_write_branch) > PATH_MAX) {
		return -ENAMETOOLONG;
	}

	/* Copy path (and /) */
	strncat(wh_path, path, tree_path - path + 1);
	/* Append me */
	strcat(wh_path, ".wh.");
	/* Finally copy name */
	strcat(wh_path, tree_path + 1);

	/* Does it exists */
	return vfs_lstat(wh_path, &kstbuf);
}

int hide_directory_contents(const char *path) {
	return -EINVAL;
}

int is_empty_dir(const char *path, const char *ro_path, const char *rw_path) {
	return -EINVAL;
}

int unlink_rw_file(const char *path, const char *rw_path, char has_ro_sure) {
	int err;
	char has_ro = 0;
	char ro_path[PATH_MAX];
	char wh_path[PATH_MAX];
	struct dentry *dentry;


	/* Check if RO exists */
	if (!has_ro_sure && find_file(path, ro_path, MUST_READ_ONLY) >= 0) {
		has_ro = 1;
	}
	else if (has_ro_sure) {
		has_ro = 1;
	}

	/* Check if user can unlink file */
	err = can_remove(path, rw_path);
	if (err < 0) {
		return err;
	}

	/* Get file dentry */
	dentry = get_path_dentry(rw_path, LOOKUP_REVAL);
	if (IS_ERR(dentry)) {
		return PTR_ERR(dentry);
	}

	/* Remove file */
	err = vfs_unlink(dentry->d_inode, dentry);
	dput(dentry);

	if (err < 0) {
		return err;
	}

	/* Whiteout potential RO file */
	if (has_ro) {
		create_whiteout(path, wh_path);
	}

	return 0;
}

int unlink_whiteout(const char *path) {
	int err;
	char wh_path[PATH_MAX];
	struct dentry *dentry;

	/* Find name */
	char *tree_path = strrchr(path, '/');
	if (!tree_path) {
		return -EINVAL;
	}

	if (snprintf(wh_path, PATH_MAX, "%s", get_context()->read_write_branch) > PATH_MAX) {
		return -ENAMETOOLONG;
	}

	/* Copy path (and /) */
	strncat(wh_path, path, tree_path - path + 1);
	/* Append wh */
	strcat(wh_path, ".wh.");
	/* Finalement copy name */
	strcat(wh_path, tree_path + 1);

	/* Get file dentry */
	dentry = get_path_dentry(wh_path, LOOKUP_REVAL);
	if (IS_ERR(dentry)) {
		return PTR_ERR(dentry);
	}

	/* Now unlink whiteout */
	err = vfs_unlink(dentry->d_inode, dentry);
	dput(dentry);

	return err;
}

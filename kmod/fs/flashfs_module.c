// SPDX-License-Identifier: GPL-2.0
/*
 * Xbox 360 flash filesystem as a kernel filesystem.
 *
 * Mount a block device path or an MTD name:
 *
 *   mount -t flashfs /dev/xenonflash0 /mnt             read-only (default)
 *   mount -t flashfs -o write /dev/xenonflash0 /mnt    read-write
 *   mount -t flashfs xenon-nand-raw /mnt               (MTD, raw NAND dump view)
 *
 * Read-only is the default even if "ro" is not given. "write" is accepted only
 * when the storage is writable (see xenon_sfcx allow_write).
 *
 * Read-write semantics. Every operation is applied to the
 * flash before the system call returns: there is no write-back cache, no
 * buffering and file data is never copied.
 *  - overwriting existing bytes is done in place;
 *  - growing a file (write past EOF, truncate up) allocates only the new,
 *    erased blocks, commits the new size, then writes the data in place;
 *  - truncating down releases the tail blocks;
 *  - unlink and rename change only the file table. The table is committed with
 *    a new root block and one slot sector write (see flashfs_write.h);
 *  - on NAND "in place" means erase + reprogram of the same physical block
 *    (spare kept), never a copy elsewhere, and the table commit is a new root
 *    block with the spare sequence number + 1 (see flashfs_write.h);
 *  - NOT supported, because they cannot be done instantly: creating files (the
 *    table cannot represent an empty file, and create + first write are two
 *    syscalls), truncating to zero, directories, symlinks, hardlinks. New files
 *    are added with the `flashfs put` tool, which creates them in one step.
 */
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mnt_idmapping.h>
#include <linux/mutex.h>
#include <linux/uio.h>
#include <linux/vmalloc.h>
#include <linux/xarray.h>
#include "flashfs_priv.h"
#include "flashfs_write.h"
#include "flashfs_dev.h"

#define FLASHFS_MAGIC 0x58464653

struct ffile {
    struct list_head list;
    unsigned long ino;
    char name[FLASHFS_NAME_LEN + 1];
    bool unlinked;
    flashfs_entry_t ent;         /* flash view (block, size) */
};

struct flashfs_sb {
    flashfs_t fs;
    struct flashfs_dev *dev;
    bool rw;
    struct mutex lock;           /* serialises every access to wr/files */
    flashfs_wr_t wr;
    struct list_head files;
    struct xarray inos;
    unsigned long next_ino;
};

struct flashfs_fc {
    bool write;
    struct flashfs_dev *dev;        /* opened in get_tree, handed to the sb */
};

enum { Opt_write };
static const struct fs_parameter_spec flashfs_params[] = {
    fsparam_flag("write", Opt_write),
    {}
};

static int flashfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
    struct flashfs_fc *o = fc->fs_private;
    struct fs_parse_result res;
    int opt = fs_parse(fc, flashfs_params, param, &res);

    if (opt < 0)
        return opt;
    if (opt == Opt_write)
        o->write = true;
    return 0;
}

static int fd_image_read(void *ctx, u64 offset, void *buf, size_t len)
{
    struct flashfs_dev *dev = ctx;

    if (offset + len > dev->size)
        return -EIO;
    return dev->read(dev, offset, buf, len);
}

static int fd_image_erase(void *ctx, u32 phys)
{
    struct flashfs_dev *dev = ctx;
    u64 bstride = (u64)dev->pages_block * (dev->page_data + dev->page_spare);

    return dev->erase(dev, phys * bstride, bstride);
}

static int fd_image_write(void *ctx, u64 offset, const void *buf, size_t len)
{
    struct flashfs_dev *dev = ctx;

    return dev->write(dev, offset, buf, len);
}

static int flashfs_load(struct flashfs_sb *st)
{
    flashfs_t *fs = &st->fs;
    struct flashfs_dev *dev = st->dev;

    fs->io_context = dev;
    fs->image_read = fd_image_read;
    fs->emmc = !dev->page_spare;                  /* flat image */
    if (fs->emmc) {
        fs->nblocks = min_t(u64, dev->size, EMMC_FLASH_SIZE) / FLASHFS_BLOCK_LEN;
    } else {                                      /* raw NAND dump view */
        fs->page_data = dev->page_data;
        fs->page_spare = dev->page_spare;
        fs->pages_block = dev->pages_block;
        fs->phys_blocks = div_u64(dev->size, (dev->page_data + dev->page_spare) *
                                  dev->pages_block);
        fs->spare = dev->spare_type;
    }
    return flashfs_open(fs);
}

/* ---- in-memory file model ---------------------------------------------- */

static struct ffile *ffile_find(struct flashfs_sb *st, const char *name,
                                unsigned int len)
{
    struct ffile *f;

    list_for_each_entry(f, &st->files, list)
        if (!f->unlinked && strlen(f->name) == len &&
            !strncasecmp(f->name, name, len))
            return f;
    return NULL;
}

static struct ffile *ffile_new(struct flashfs_sb *st, const char *name)
{
    struct ffile *f = kzalloc(sizeof(*f), GFP_KERNEL);

    if (!f)
        return NULL;
    strscpy(f->name, name, sizeof(f->name));
    f->ino = st->next_ino++;
    if (xa_err(xa_store(&st->inos, f->ino, f, GFP_KERNEL))) {
        kfree(f);
        return NULL;
    }
    list_add_tail(&f->list, &st->files);
    return f;
}

static void ffile_free(struct flashfs_sb *st, struct ffile *f)
{
    list_del(&f->list);
    xa_erase(&st->inos, f->ino);
    kfree(f);
}

/* Re-read what the reader needs from the (committed) working root. */
static void refresh_view(struct flashfs_sb *st)
{
    struct ffile *f;

    flashfs_wr_export_map(&st->wr, st->fs.blockmap);
    list_for_each_entry(f, &st->files, list) {
        u16 start;
        u32 size;

        if (!f->unlinked &&
            !flashfs_wr_lookup(&st->wr, f->name, &start, &size)) {
            f->ent.block = start;
            f->ent.size = size;
        }
    }
}

/* Commit the pending table change now; roll back on failure. */
static int commit(struct flashfs_sb *st)
{
    int ret = flashfs_wr_commit(&st->wr);

    if (ret)
        flashfs_wr_abort(&st->wr);
    else
        refresh_view(st);
    return ret;
}

/* ---- file operations --------------------------------------------------- */

static ssize_t flashfs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct inode *inode = file_inode(iocb->ki_filp);
    struct flashfs_sb *st = inode->i_sb->s_fs_info;
    struct ffile *f = inode->i_private;
    loff_t pos = iocb->ki_pos;
    size_t len = min_t(size_t, iov_iter_count(to), FLASHFS_BLOCK_LEN);
    size_t copied;
    ssize_t ret = 0;
    u8 *tmp;
    int n;

    if (pos < 0)
        return -EINVAL;
    mutex_lock(&st->lock);
    if (pos >= f->ent.size || !len)
        goto out;
    len = min_t(size_t, len, f->ent.size - pos);
    tmp = kmalloc(len, GFP_KERNEL);
    if (!tmp) {
        ret = -ENOMEM;
        goto out;
    }
    n = _flashfs_read(&st->fs, &f->ent, tmp, pos, len);
    if (n < 0) {
        ret = -EIO;
    } else {
        copied = copy_to_iter(tmp, n, to);
        if (!copied) {
            ret = -EFAULT;
        } else {
            iocb->ki_pos += copied;
            ret = copied;
        }
    }
    kfree(tmp);
out:
    mutex_unlock(&st->lock);
    return ret;
}

/* In-place overwrite inside the existing size: follow the 16K block chain. */
static ssize_t write_in_place(struct flashfs_sb *st, struct ffile *f,
                              struct iov_iter *from, loff_t pos)
{
    flashfs_t *fs = &st->fs;
    ssize_t done = 0;
    u8 *buf = kmalloc(FLASHFS_BLOCK_LEN, GFP_KERNEL);

    if (!buf)
        return -ENOMEM;
    while (iov_iter_count(from)) {
        u32 idx = div_u64(pos, FLASHFS_BLOCK_LEN), in = pos % FLASHFS_BLOCK_LEN;
        size_t n = min_t(size_t, iov_iter_count(from), FLASHFS_BLOCK_LEN - in);
        u32 cur = f->ent.block, guard = 0;
        int ret;

        while (idx--) {
            cur = fs->blockmap[cur] & 0x7fffu;
            if (cur == FLASHFS_BLK_FREE || cur == FLASHFS_BLK_END ||
                cur >= fs->nblocks || ++guard > fs->nblocks) {
                done = done ? done : -EIO;
                goto out;
            }
        }
        if (copy_from_iter(buf, n, from) != n) {
            done = done ? done : -EFAULT;
            break;
        }
        ret = flashfs_wr_patch(&st->wr, cur + fs->fs_offset, in, buf, n);
        if (ret) {
            done = done ? done : ret;
            break;
        }
        pos += n;
        done += n;
    }
out:
    kfree(buf);
    return done;
}

/* Grow a file to `ns` bytes right now: allocate and zero only the new blocks
 * and commit the new size. Existing data is not touched. */
static int grow_now(struct flashfs_sb *st, struct ffile *f, u32 ns)
{
    int ret;

    st->wr.now = (u32)ktime_get_real_seconds();
    ret = flashfs_wr_resize(&st->wr, f->name, ns);
    if (!ret)
        ret = commit(st);
    else
        flashfs_wr_abort(&st->wr);
    return ret;
}

static ssize_t flashfs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct inode *inode = file_inode(iocb->ki_filp);
    struct flashfs_sb *st = inode->i_sb->s_fs_info;
    struct ffile *f = inode->i_private;
    loff_t pos;
    size_t count;
    ssize_t ret;

    if (!st->rw)
        return -EROFS;
    inode_lock(inode);
    mutex_lock(&st->lock);
    pos = iocb->ki_flags & IOCB_APPEND ? f->ent.size : iocb->ki_pos;
    count = iov_iter_count(from);
    if (pos < 0 || (u64)pos + count > 0xffffffffu) {
        ret = -EFBIG;
        goto out;
    }
    if (!count) {
        ret = 0;
        goto out;
    }
    if (pos + count > f->ent.size) {
        /* extend first (new blocks only), then write everything in place */
        ret = grow_now(st, f, pos + count);
        if (ret)
            goto out;
        i_size_write(inode, f->ent.size);
    }
    ret = write_in_place(st, f, from, pos);
    if (ret > 0)
        iocb->ki_pos = pos + ret;
out:
    mutex_unlock(&st->lock);
    inode_unlock(inode);
    return ret;
}

static int flashfs_truncate(struct flashfs_sb *st, struct ffile *f, u32 ns)
{
    int ret;

    if (ns == f->ent.size)
        return 0;
    if (!ns)
        return -EPERM;               /* empty files are not representable */
    st->wr.now = (u32)ktime_get_real_seconds();
    ret = flashfs_wr_resize(&st->wr, f->name, ns);
    if (!ret)
        ret = commit(st);
    else
        flashfs_wr_abort(&st->wr);
    return ret;
}

static int flashfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
                           struct iattr *attr)
{
    struct inode *inode = d_inode(dentry);
    struct flashfs_sb *st = inode->i_sb->s_fs_info;
    int ret;

    ret = setattr_prepare(idmap, dentry, attr);
    if (ret)
        return ret;
    if (attr->ia_valid & ATTR_SIZE) {
        if (!st->rw)
            return -EROFS;
        mutex_lock(&st->lock);
        ret = flashfs_truncate(st, inode->i_private, attr->ia_size);
        mutex_unlock(&st->lock);
        if (ret)
            return ret;
        truncate_setsize(inode, attr->ia_size);
    }
    setattr_copy(idmap, inode, attr);
    mark_inode_dirty(inode);
    return 0;
}

static const struct inode_operations flashfs_file_iops = {
    .setattr = flashfs_setattr,
};

static const struct file_operations flashfs_file_ops = {
    .owner = THIS_MODULE,
    .read_iter = flashfs_read_iter,
    .write_iter = flashfs_write_iter,
    .llseek = generic_file_llseek,
    .fsync = noop_fsync,             /* everything is already on flash */
};

/* ---- inodes and directory ---------------------------------------------- */

static const struct inode_operations flashfs_dir_iops;
static const struct file_operations flashfs_dir_fops;

static struct inode *flashfs_iget(struct super_block *sb, unsigned long ino)
{
    struct flashfs_sb *st = sb->s_fs_info;
    struct inode *inode = iget_locked(sb, ino);
    struct ffile *f;

    if (!inode)
        return ERR_PTR(-ENOMEM);
    if (!(inode->i_state & I_NEW))
        return inode;
    inode->i_uid = GLOBAL_ROOT_UID;
    inode->i_gid = GLOBAL_ROOT_GID;
    if (ino == 1) {
        inode->i_mode = S_IFDIR | 0555 | (st->rw ? 0200 : 0);
        inode->i_op = &flashfs_dir_iops;
        inode->i_fop = &flashfs_dir_fops;
        set_nlink(inode, 2);
    } else {
        f = xa_load(&st->inos, ino);
        if (!f) {
            iget_failed(inode);
            return ERR_PTR(-ENOENT);
        }
        inode->i_mode = S_IFREG | (st->rw ? 0644 : 0444);
        inode->i_size = f->ent.size;
        inode->i_private = f;
        inode->i_op = &flashfs_file_iops;
        inode->i_fop = &flashfs_file_ops;
        set_nlink(inode, 1);
    }
    unlock_new_inode(inode);
    return inode;
}

static struct dentry *flashfs_lookup(struct inode *dir, struct dentry *dentry,
                                     unsigned int flags)
{
    struct flashfs_sb *st = dir->i_sb->s_fs_info;
    struct inode *inode = NULL;
    struct ffile *f;

    mutex_lock(&st->lock);
    f = ffile_find(st, dentry->d_name.name, dentry->d_name.len);
    if (f) {
        inode = flashfs_iget(dir->i_sb, f->ino);
        if (IS_ERR(inode)) {
            mutex_unlock(&st->lock);
            return ERR_CAST(inode);
        }
    }
    mutex_unlock(&st->lock);
    return d_splice_alias(inode, dentry);
}

static int flashfs_iterate(struct file *file, struct dir_context *ctx)
{
    struct flashfs_sb *st = file_inode(file)->i_sb->s_fs_info;
    struct ffile *f;
    loff_t idx = 0;

    if (!dir_emit_dots(file, ctx))
        return 0;
    mutex_lock(&st->lock);
    list_for_each_entry(f, &st->files, list) {
        if (f->unlinked)
            continue;
        if (idx++ + 2 < ctx->pos)
            continue;
        if (!dir_emit(ctx, f->name, strlen(f->name), f->ino, DT_REG))
            break;
        ctx->pos = idx + 2;
    }
    mutex_unlock(&st->lock);
    return 0;
}

/* Not supported: the file table cannot hold an empty file, so a bare create
 * cannot be made instant. Use `flashfs put` to add a new file in one step. */
static int flashfs_create(struct mnt_idmap *idmap, struct inode *dir,
                          struct dentry *dentry, umode_t mode, bool excl)
{
    return -EPERM;
}

static int flashfs_unlink(struct inode *dir, struct dentry *dentry)
{
    struct flashfs_sb *st = dir->i_sb->s_fs_info;
    struct inode *inode = d_inode(dentry);
    struct ffile *f = inode->i_private;
    int ret = 0;

    if (!st->rw)
        return -EROFS;
    mutex_lock(&st->lock);
    st->wr.now = (u32)ktime_get_real_seconds();
    ret = flashfs_wr_rm(&st->wr, f->name);
    if (!ret)
        ret = commit(st);
    else
        flashfs_wr_abort(&st->wr);
    if (!ret) {
        f->unlinked = true;
        drop_nlink(inode);
    }
    mutex_unlock(&st->lock);
    return ret;
}

static int flashfs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
                          struct dentry *old_dentry, struct inode *new_dir,
                          struct dentry *new_dentry, unsigned int flags)
{
    struct flashfs_sb *st = old_dir->i_sb->s_fs_info;
    struct ffile *f = d_inode(old_dentry)->i_private, *g;
    char name[FLASHFS_NAME_LEN + 2];
    int ret = 0;

    if (!st->rw)
        return -EROFS;
    if (flags & ~RENAME_NOREPLACE)
        return -EINVAL;
    if (new_dentry->d_name.len > FLASHFS_NAME_LEN)
        return -ENAMETOOLONG;
    memcpy(name, new_dentry->d_name.name, new_dentry->d_name.len);
    name[new_dentry->d_name.len] = 0;
    if (!flashfs_name_valid(name))
        return -EINVAL;
    mutex_lock(&st->lock);
    g = ffile_find(st, name, new_dentry->d_name.len);
    st->wr.now = (u32)ktime_get_real_seconds();
    if (g && g != f) {
        if (flags & RENAME_NOREPLACE) {
            ret = -EEXIST;
            goto out;
        }
        ret = flashfs_wr_rm(&st->wr, g->name);     /* replace target ... */
        if (ret)
            goto fail;
    }
    if (strcmp(f->name, name)) {
        ret = flashfs_wr_rename(&st->wr, f->name, name);   /* ... and rename */
        if (ret)
            goto fail;
    }
    ret = commit(st);                                /* one commit for both */
    if (ret)
        goto out;
    if (g && g != f) {
        g->unlinked = true;
        if (d_really_is_positive(new_dentry))
            drop_nlink(d_inode(new_dentry));
    }
    strscpy(f->name, name, sizeof(f->name));
    refresh_view(st);
    goto out;
fail:
    flashfs_wr_abort(&st->wr);
out:
    mutex_unlock(&st->lock);
    return ret;
}

static const struct inode_operations flashfs_dir_iops = {
    .lookup = flashfs_lookup,
    .create = flashfs_create,
    .unlink = flashfs_unlink,
    .rename = flashfs_rename,
};

static const struct file_operations flashfs_dir_fops = {
    .owner = THIS_MODULE,
    .llseek = generic_file_llseek,
    .read = generic_read_dir,
    .iterate_shared = flashfs_iterate,
    .fsync = noop_fsync,
};

/* ---- super block ------------------------------------------------------- */

static void flashfs_evict_inode(struct inode *inode)
{
    struct flashfs_sb *st = inode->i_sb->s_fs_info;
    struct ffile *f = inode->i_private;

    truncate_inode_pages_final(&inode->i_data);
    clear_inode(inode);
    if (f && f->unlinked) {
        mutex_lock(&st->lock);
        ffile_free(st, f);
        mutex_unlock(&st->lock);
    }
}

static const struct super_operations flashfs_super_ops = {
    .statfs = simple_statfs,
    .evict_inode = flashfs_evict_inode,
};

static void flashfs_free(struct flashfs_sb *st)
{
    struct ffile *f, *n;

    list_for_each_entry_safe(f, n, &st->files, list)
        ffile_free(st, f);
    xa_destroy(&st->inos);
    if (st->rw)
        flashfs_wr_close(&st->wr);
    flashfs_close(&st->fs);
    kfree(st);
}

static int flashfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
    struct flashfs_fc *o = fc->fs_private;
    struct flashfs_dev *dev = o->dev;
    struct flashfs_sb *st;
    struct inode *root;
    u32 i;
    int ret;

    st = kzalloc(sizeof(*st), GFP_KERNEL);
    if (!st)
        return -ENOMEM;
    st->dev = dev;
    mutex_init(&st->lock);
    INIT_LIST_HEAD(&st->files);
    xa_init(&st->inos);
    st->next_ino = 2;
    ret = flashfs_load(st);
    if (ret)
        goto err;
    if (o->write) {
        ret = flashfs_wr_open(&st->wr, &st->fs, fd_image_write,
                              dev->erase ? fd_image_erase : NULL, dev,
                              (u32)ktime_get_real_seconds());
        if (ret) {
            pr_err("flashfs: cannot enable writing (%d): need a valid newest "
                   "root (eMMC slot or NAND root)\n", ret);
            ret = -EROFS;
            goto err;
        }
        st->rw = true;
    }
    for (i = 0; i < st->fs.nentries; i++) {
        struct ffile *f = ffile_new(st, st->fs.entries[i].name);

        if (!f) {
            ret = -ENOMEM;
            goto err;
        }
        f->ent = st->fs.entries[i];
    }
    sb->s_fs_info = st;
    sb->s_magic = FLASHFS_MAGIC;
    sb->s_blocksize = FLASHFS_BLOCK_LEN;
    sb->s_blocksize_bits = 14;
    sb->s_maxbytes = 0xffffffffu;
    sb->s_op = &flashfs_super_ops;
    if (!st->rw)
        sb->s_flags |= SB_RDONLY;   /* default: read-only regardless of "ro" */

    root = flashfs_iget(sb, 1);
    if (IS_ERR(root))
        return PTR_ERR(root);
    sb->s_root = d_make_root(root);
    if (!sb->s_root)
        return -ENOMEM;
    pr_info("flashfs: mounted %s (%u files, %s)\n", dev->name,
            st->fs.nentries, st->rw ? "READ-WRITE" : "read-only");
    return 0;
err:
    flashfs_free(st);
    return ret;
}

static int flashfs_get_tree(struct fs_context *fc)
{
    struct flashfs_fc *o = fc->fs_private;
    int ret;

    if (!fc->source)
        return invalfc(fc, "no flash device given (/dev/... or an MTD name)");
    o->dev = flashfs_dev_open(fc->source, o->write);
    if (IS_ERR(o->dev)) {
        ret = PTR_ERR(o->dev);
        o->dev = NULL;
        switch (ret) {
        case -EBUSY:
            return invalfc(fc, "%s is in use (a writer needs the device to itself)",
                           fc->source);
        case -ENODEV:
            return invalfc(fc, "no such flash device: %s", fc->source);
        case -EROFS:
        case -EACCES:
            return invalfc(fc, "%s is not writable (load xenon_sfcx with "
                           "allow_write=1)", fc->source);
        }
        return invalfc(fc, "cannot open %s: error %d", fc->source, ret);
    }
    ret = get_tree_nodev(fc, flashfs_fill_super);
    if (ret) {
        flashfs_dev_close(o->dev);
        o->dev = NULL;
    }
    return ret;
}

static void flashfs_free_fc(struct fs_context *fc)
{
    kfree(fc->fs_private);                 /* the device belongs to the sb now */
}

static const struct fs_context_operations flashfs_context_ops = {
    .parse_param = flashfs_parse_param,
    .get_tree = flashfs_get_tree,
    .free = flashfs_free_fc,
};

static int flashfs_init_fs_context(struct fs_context *fc)
{
    fc->fs_private = kzalloc(sizeof(struct flashfs_fc), GFP_KERNEL);
    if (!fc->fs_private)
        return -ENOMEM;
    fc->ops = &flashfs_context_ops;
    return 0;
}

static void flashfs_kill_sb(struct super_block *sb)
{
    struct flashfs_sb *st = sb->s_fs_info;

    kill_anon_super(sb);
    if (st) {
        struct flashfs_dev *dev = st->dev;

        sb->s_fs_info = NULL;
        flashfs_free(st);
        flashfs_dev_close(dev);
    }
}

static struct file_system_type flashfs_type = {
    .owner = THIS_MODULE,
    .name = "flashfs",
    .init_fs_context = flashfs_init_fs_context,
    .parameters = flashfs_params,
    .kill_sb = flashfs_kill_sb,
};

static int __init flashfs_init(void)
{
    return register_filesystem(&flashfs_type);
}

static void __exit flashfs_exit(void)
{
    unregister_filesystem(&flashfs_type);
}

module_init(flashfs_init);
module_exit(flashfs_exit);
MODULE_DESCRIPTION("Xbox 360 flash filesystem (read-only by default)");
MODULE_LICENSE("GPL");

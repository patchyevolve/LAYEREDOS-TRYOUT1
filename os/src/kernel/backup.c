#include "kernel.h"
#include "backup.h"
#include "vfs.h"
#include "kmalloc.h"
#include "pmm.h"

#define BACKUP_MAGIC 0x424B505F
#define BUF_SIZE 4096

typedef struct {
    uint32_t type;
    uint32_t mode;
    uint32_t name_len;
    uint32_t data_len;
} __attribute__((packed)) backup_hdr_t;

enum {
    BACKUP_FILE = 0,
    BACKUP_DIR  = 1,
    BACKUP_LINK = 2,
    BACKUP_END  = 0xFFFFFFFF
};

static int backup_write_all(int fd, const void* buf, uint32_t len) {
    uint64_t written = 0;
    while (written < len) {
        int64_t n = vfs_write(fd, (const uint8_t*)buf + written, len - written);
        if (n <= 0) return -1;
        written += (uint64_t)n;
    }
    return 0;
}

static int backup_read_all(int fd, void* buf, uint32_t len) {
    uint64_t read = 0;
    while (read < len) {
        int64_t n = vfs_read(fd, (uint8_t*)buf + read, len - read);
        if (n <= 0) return -1;
        read += (uint64_t)n;
    }
    return 0;
}

/* Recursively walk a directory and write entries to the archive */
static int backup_walk(int afd, const char* rel_path, vfs_node_t* node) {
    if (!node) return -1;

    ssize_t path_len = kstrlen(rel_path);

    if (node->flags & 1) {
        /* Directory */
        backup_hdr_t hdr;
        hdr.type = BACKUP_DIR;
        hdr.mode = 0755;
        hdr.name_len = (uint32_t)path_len + 1;
        hdr.data_len = 0;

        if (backup_write_all(afd, &hdr, sizeof(hdr))) return -1;
        if (backup_write_all(afd, rel_path, hdr.name_len)) return -1;

        /* Iterate children */
        uint32_t idx = 0;
        while (1) {
            vfs_node_t* child = NULL;
            if (!node->fs || !node->fs->ops || !node->fs->ops->readdir) break;
            if (node->fs->ops->readdir(node, idx, &child) != 0 || !child) break;
            idx++;

            char* child_path = (char*)kmalloc((size_t)(path_len + 1 + VFS_MAX_NAME + 1));
            if (!child_path) return -1;
            kmemcpy(child_path, rel_path, (size_t)path_len);
            child_path[path_len] = '/';
            kstrncpy(child_path + path_len + 1, child->name, VFS_MAX_NAME);

            int r = backup_walk(afd, child_path, child);
            kfree(child_path);
            if (r) return r;
        }
    } else if (node->flags & VFS_FLAG_SYMLINK) {
        /* Symlink */
        char target[256];
        kmemset(target, 0, sizeof(target));
        if (node->fs && node->fs->ops && node->fs->ops->readlink) {
            if (node->fs->ops->readlink(node, target, sizeof(target)) != 0) return -1;
        }

        uint32_t tlen = (uint32_t)kstrlen(target) + 1;
        backup_hdr_t hdr;
        hdr.type = BACKUP_LINK;
        hdr.mode = 0777;
        hdr.name_len = (uint32_t)path_len + 1;
        hdr.data_len = tlen;

        if (backup_write_all(afd, &hdr, sizeof(hdr))) return -1;
        if (backup_write_all(afd, rel_path, hdr.name_len)) return -1;
        if (backup_write_all(afd, target, tlen)) return -1;
    } else {
        /* Regular file */
        vfs_stat_t st;
        if (vfs_stat(rel_path, &st) != 0) return -1;

        backup_hdr_t hdr;
        hdr.type = BACKUP_FILE;
        hdr.mode = st.mode & 0xFFFF;
        hdr.name_len = (uint32_t)path_len + 1;
        hdr.data_len = (uint32_t)st.size;

        if (backup_write_all(afd, &hdr, sizeof(hdr))) return -1;
        if (backup_write_all(afd, rel_path, hdr.name_len)) return -1;

        /* Write file contents */
        int fdf = vfs_open(rel_path, O_RDONLY);
        if (fdf < 0) return -1;

        uint8_t* buf = (uint8_t*)kmalloc(BUF_SIZE);
        if (!buf) { vfs_close(fdf); return -1; }

        uint64_t remain = st.size;
        while (remain > 0) {
            uint32_t chunk = (uint32_t)(remain < BUF_SIZE ? remain : BUF_SIZE);
            int64_t nr = vfs_read(fdf, buf, chunk);
            if (nr <= 0) break;
            if (backup_write_all(afd, buf, (uint32_t)nr)) { kfree(buf); vfs_close(fdf); return -1; }
            remain -= (uint64_t)nr;
        }
        kfree(buf);
        vfs_close(fdf);
    }

    return 0;
}

int backup_create(const char* archive_path) {
    kprintf("[BACKUP] Creating archive '%s'...\n", archive_path);

    int afd = vfs_open(archive_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (afd < 0) {
        kprintf("[BACKUP] Cannot create archive '%s'\n", archive_path);
        return -1;
    }

    /* Write magic */
    uint32_t magic = BACKUP_MAGIC;
    backup_write_all(afd, &magic, sizeof(magic));

    /* Walk from root */
    int r = backup_walk(afd, "", vfs_find("/"));

    /* Write end marker */
    backup_hdr_t end;
    end.type = BACKUP_END;
    end.mode = 0;
    end.name_len = 0;
    end.data_len = 0;
    backup_write_all(afd, &end, sizeof(end));

    vfs_close(afd);

    if (r) {
        kprintf("[BACKUP] Failed during backup\n");
        return r;
    }

    /* Check archive size */
    vfs_stat_t st;
    vfs_stat(archive_path, &st);
    kprintf("[BACKUP] Done: %llu bytes\n", st.size);
    return 0;
}

int backup_restore(const char* archive_path) {
    kprintf("[BACKUP] Restoring from '%s'...\n", archive_path);

    int afd = vfs_open(archive_path, O_RDONLY);
    if (afd < 0) {
        kprintf("[BACKUP] Cannot open archive '%s'\n", archive_path);
        return -1;
    }

    /* Check magic */
    uint32_t magic;
    if (backup_read_all(afd, &magic, sizeof(magic)) || magic != BACKUP_MAGIC) {
        kprintf("[BACKUP] Bad magic\n");
        vfs_close(afd);
        return -1;
    }

    uint32_t restored = 0;
    while (1) {
        backup_hdr_t hdr;
        if (backup_read_all(afd, &hdr, sizeof(hdr))) break;

        if (hdr.type == BACKUP_END) break;

        if (hdr.name_len == 0) continue;

        char* name = (char*)kmalloc((size_t)hdr.name_len + 1);
        if (!name) { vfs_close(afd); return -1; }
        if (backup_read_all(afd, name, hdr.name_len)) { kfree(name); break; }
        name[hdr.name_len] = '\0';

        /* Skip empty names (root dir itself) */
        if (name[0] == '\0') { kfree(name); continue; }

        if (hdr.type == BACKUP_DIR) {
            if (vfs_mkdir(name) != 0 && vfs_stat(name, NULL) != 0) {
                kprintf("[BACKUP] Warning: could not create dir '%s'\n", name);
            }
            kfree(name);
            restored++;
        } else if (hdr.type == BACKUP_LINK) {
            char* target = (char*)kmalloc((size_t)hdr.data_len + 1);
            if (!target) { kfree(name); break; }
            if (backup_read_all(afd, target, hdr.data_len)) { kfree(target); kfree(name); break; }
            target[hdr.data_len] = '\0';
            vfs_unlink(name);
            vfs_symlink(target, name);
            kfree(target);
            kfree(name);
            restored++;
        } else {
            /* Regular file */
            uint8_t* data = NULL;
            if (hdr.data_len > 0) {
                data = (uint8_t*)kmalloc(hdr.data_len);
                if (!data) { kfree(name); break; }
                if (backup_read_all(afd, data, hdr.data_len)) { kfree(data); kfree(name); break; }
            }

            vfs_unlink(name);
            int fdf = vfs_open(name, O_WRONLY | O_CREAT | O_TRUNC);
            if (fdf >= 0) {
                if (hdr.data_len > 0)
                    vfs_write(fdf, data, hdr.data_len);
                vfs_close(fdf);
                if (hdr.mode)
                    vfs_chmod(name, hdr.mode);
            }
            if (data) kfree(data);
            kfree(name);
            restored++;
        }
    }

    vfs_close(afd);
    kprintf("[BACKUP] Restore complete: %u entries restored\n", restored);
    return 0;
}

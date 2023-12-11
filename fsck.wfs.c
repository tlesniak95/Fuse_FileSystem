#include "wfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int disk_fd = -1;
struct wfs_sb sb;

struct wfs_log_entry *read_log_entry(int fd, off_t offset) {
    struct wfs_inode inode;
    if (pread(fd, &inode, sizeof(struct wfs_inode), offset) != sizeof(struct wfs_inode)) {
        perror("Error reading inode");
        return NULL;
    }

    size_t log_entry_size = sizeof(struct wfs_inode) + inode.size;
    struct wfs_log_entry *entry = (struct wfs_log_entry *)malloc(log_entry_size);
    if (!entry) {
        perror("Error allocating memory for log entry");
        return NULL;
    }

    if (pread(fd, entry, log_entry_size, offset) != log_entry_size) {
        perror("Error reading log entry");
        free(entry);
        return NULL;
    }

    return entry;
}

int write_log_entry(int fd, struct wfs_log_entry *entry, off_t offset) {
    size_t entry_size = sizeof(struct wfs_inode) + entry->inode.size;
    if (pwrite(fd, entry, entry_size, offset) != entry_size) {
        perror("Error writing log entry");
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <disk image>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *disk_path = argv[1];
    disk_fd = open(disk_path, O_RDWR);
    if (disk_fd == -1) {
        perror("Error opening disk");
        exit(EXIT_FAILURE);
    }

    if (pread(disk_fd, &sb, sizeof(sb), 0) != sizeof(sb)) {
        perror("Error reading superblock");
        close(disk_fd);
        return -1;
    }

    if (sb.magic != WFS_MAGIC) {
        fprintf(stderr, "Invalid filesystem magic number\n");
        close(disk_fd);
        return -1;
    }

    off_t current_offset = sizeof(struct wfs_sb);
    off_t new_offset = current_offset;

    while (current_offset < sb.head) {
        struct wfs_log_entry *entry = read_log_entry(disk_fd, current_offset);
        if (!entry) {
            close(disk_fd);
            return -1;
        }

        if (!entry->inode.deleted) {
            if (write_log_entry(disk_fd, entry, new_offset) != 0) {
                free(entry);
                close(disk_fd);
                return -1;
            }
            new_offset += sizeof(struct wfs_inode) + entry->inode.size;
        }

        current_offset += sizeof(struct wfs_inode) + entry->inode.size;
        free(entry);
    }

    sb.head = new_offset;
    if (pwrite(disk_fd, &sb, sizeof(sb), 0) != sizeof(sb)) {
        perror("Error updating superblock");
        close(disk_fd);
        return -1;
    }

    close(disk_fd);
    printf("Filesystem compaction completed successfully.\n");

    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "wfs.h"
#include <fcntl.h>

int main (int argc, char *argv[]) {
    if (argc != 2) {
        printf("expected 2 arguments, got %d\n", argc);
        exit(1);
    }
    const char *disk_path = argv[1];
    int fd = open(disk_path, O_RDWR);
    if (fd == -1) {
        printf("error opening disk\n");
        return 1;
    }

    struct wfs_inode root_inode = {
        .inode_number = 0,
        .mode = S_IFDIR  | 0755,
        .uid = getuid(),
        .gid = getgid(),
        .size = 0,
        .links = 1,
        .deleted = 0,
    };

    struct wfs_log_entry root_entry = {
        .inode = root_inode
        // .data is empty since this is a new directory
    };
    
    //initialze and update superblock
    struct wfs_sb sb = {WFS_MAGIC, sizeof(struct wfs_sb) + sizeof(root_entry)};
    lseek(fd, 0, SEEK_SET);
    if (write(fd, &sb, sizeof(sb)) != sizeof(sb)) {
        perror("Error updating superblock");
        close(fd);
        return 1;
    }
    //write root inode
    if(write(fd, &root_entry, sizeof(root_entry)) != sizeof(root_entry)) {
        printf("error writing root inode\n");
        close(fd);
        return 1;
    }



    // Close the disk file
    close(fd);

    return 0;
}
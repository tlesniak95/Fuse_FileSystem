//initialize the file system, writing the super block
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

    //initialize and write super block
    struct wfs_sb sb = {
        WFS_MAGIC,
        sizeof(struct wfs_sb)
    };
    if (write(fd, &sb, sizeof(sb)) != sizeof(sb)) {
        printf("error writing super block\n");
        close(fd);
        return 1;
    }

    struct wfs_inode root_inode = {
        .inode_number = 1,
        .mode = S_IFDIR,
        .uid = getuid(),
        .gid = getgid(),
        .size = 0,
        .links = 1
    };

    //write root inode
    if(write(fd, &root_inode, sizeof(root_inode)) != sizeof(root_inode)) {
        printf("error writing root inode\n");
        close(fd);
        return 1;
    }

    //update the superblock head to reflect the next empty space
    sb.head = sizeof(sb) + sizeof(root_inode);
    lseek(fd, 0, SEEK_SET);
    if (write(fd, &sb, sizeof(sb)) != sizeof(sb)) {
        perror("Error updating superblock");
        close(fd);
        return 1;
    }

    // Close the disk file
    close(fd);

    return 0;

    




}
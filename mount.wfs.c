#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include "wfs.h"
#include <stdlib.h>
#include <unistd.h>

int disk_fd = -1;

// Function to read a log entry from the disk at a given offset
struct wfs_log_entry *read_log_entry(int fd, off_t offset)
{
    // Read the inode first to determine the size of the log entry
    struct wfs_inode inode;
    if (pread(fd, &inode, sizeof(struct wfs_inode), offset) != sizeof(struct wfs_inode))
    {
        perror("Error reading inode");
        return NULL;
    }

    // Calculate the size of the entire log entry
    size_t log_entry_size = sizeof(struct wfs_inode) + inode.size; // Assuming inode.size includes the size of the data

    // Allocate memory for the log entry
    struct wfs_log_entry *entry = (struct wfs_log_entry *)malloc(log_entry_size);
    if (entry == NULL)
    {
        perror("Error allocating memory for log entry");
        return NULL;
    }

    // Read the entire log entry (inode + data) into memory
    if (pread(fd, entry, log_entry_size, offset) != log_entry_size)
    {
        perror("Error reading log entry");
        free(entry);
        return NULL;
    }

    return entry;
}


struct wfs_inode *find_inode(const char *path) {
    if (disk_fd == -1) {
        perror("Error opening filesystem image");
        return NULL;
    }

    // If the path is the root directory, handle it as a special case
    if (strcmp(path, "/") == 0) {
        struct wfs_inode *root_inode = malloc(sizeof(struct wfs_inode));
        if (root_inode == NULL) {
            perror("Error allocating memory for root inode");
            printf("Error allocating memory for root inode\n");
            return NULL;
        }

        // The root inode is the first inode after the superblock
        off_t root_inode_offset = sizeof(struct wfs_sb);
        ssize_t read_size = pread(disk_fd, root_inode, sizeof(struct wfs_inode), root_inode_offset);
        if (read_size != sizeof(struct wfs_inode)) {
            perror("Error reading root inode");
            printf("Error reading root inode\n");
            free(root_inode);
            return NULL;
        }
        //Print all the filds of root_inode
        printf("root_inode->inode_number: %d\n", root_inode->inode_number);
        printf("root_inode->mode: %d\n", root_inode->mode);
        printf("root_inode->uid: %d\n", root_inode->uid);
        printf("root_inode->gid: %d\n", root_inode->gid);
        printf("root_inode->size: %d\n", root_inode->size);
        printf("root_inode->links: %d\n", root_inode->links);
        printf("root_inode->deleted: %d\n", root_inode->deleted);
        // If the root inode is marked as deleted, it's an error
        if (root_inode->deleted) {
            free(root_inode);
            printf("Root inode is marked as deleted\n");
            return NULL;
        }

        return root_inode;
    }

    // Start after superblock
    off_t current_offset = sizeof(struct wfs_sb);

    // Tokenize the path
    char *path_copy = strdup(path);
    if (path_copy == NULL) {
        perror("Failed to duplicate path");
        printf("Failed to duplicate path\n");
        return NULL;
    }
    char *token = strtok(path_copy, "/");
    struct wfs_inode *found_inode = NULL;
    struct wfs_log_entry *entry = NULL;

    // This will hold the most recent inode for each path component
    struct wfs_inode *current_inode = NULL;

    // Traverse the entire disk
    while (1) {
        if (entry) {
            free(entry);  // Free the previous log entry
            entry = NULL;
        }

        // Read the next log entry
        entry = read_log_entry(disk_fd, current_offset);
        if (!entry) {
            // Reached the end of the log or encountered an error
            break;
        }

        // Check if this log entry is the one we're looking for
        if (current_inode == NULL || entry->inode.inode_number == current_inode->inode_number) {
            // Found an entry for the current path component
            if (found_inode) {
                free(found_inode);  // Free the old inode
            }
            found_inode = malloc(sizeof(struct wfs_inode));
            if (found_inode == NULL) {
                perror("Error allocating memory for inode");
                printf("Error allocating memory for inode\n");
                free(entry);
                free(path_copy);
                return NULL;
            }
            memcpy(found_inode, &entry->inode, sizeof(struct wfs_inode));

            // If this entry is deleted, we should invalidate the found inode
            if (entry->inode.deleted) {
                free(found_inode);
                found_inode = NULL;
                break;
            }

            // If this is the last component of the path, we're done
            if (token == NULL || *token == '\0') {
                break;
            }
        }

        // Move to the next log entry
        current_offset += sizeof(struct wfs_log_entry) + entry->inode.size;

        // Check if we need to move on to the next component of the path
        if (found_inode && S_ISDIR(found_inode->mode)) {
            struct wfs_dentry *dentries = (struct wfs_dentry *)(entry->data);
            size_t num_dentries = entry->inode.size / sizeof(struct wfs_dentry);
            for (size_t i = 0; i < num_dentries; ++i) {
                if (strcmp(dentries[i].name, token) == 0) {
                    // Move on to the next component of the path
                    token = strtok(NULL, "/");
                    current_inode = found_inode;
                    break;
                }
            }
        }
    }

    free(path_copy);  // Free the duplicated path

    if (entry) {
        free(entry); // Free the last read log entry
    }

    return found_inode;  // Return the found inode
}



/*
Return file attributes. The "stat" structure is described in detail in the stat(2) manual page.
For the given pathname, this should fill in the elements of the "stat" structure.
If a field is meaningless or semi-meaningless (e.g., st_ino) then it should be set to 0 or
given a "reasonable" value. This call is pretty much required for a usable filesystem.
*/
static int wfs_getattr(const char *path, struct stat *stbuf)
{
    //Print path
    printf("path: %s\n", path);
    //print if root directory
    memset(stbuf, 0, sizeof(struct stat)); // Clear the stat structure

    if (strcmp(path, "/") == 0) { // root directory
        stbuf->st_mode = S_IFDIR | 00777;
        stbuf->st_nlink = 2;
        printf("root directory\n");
    }
    // find inode for path
    struct wfs_inode *inode = find_inode(path);
    if (inode == NULL)
    {
        return -ENOENT; // No such file or directory
    }
    
    
    stbuf->st_mode = inode->mode;
    stbuf->st_ino = inode->inode_number;
    stbuf->st_nlink = inode->links;
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_size = inode->size;
    stbuf->st_atime = inode->atime;
    stbuf->st_mtime = inode->mtime;
    stbuf->st_ctime = inode->ctime;

    return 0; // Return 0 on success
}

static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    //.mknod      = wfs_mknod,
    //.mkdir      = wfs_mkdir,
    //.read	    = wfs_read,
    //.write      = wfs_write,
    //.readdir	= wfs_readdir,
    //.unlink    	= wfs_unlink,
};

int main(int argc, char *argv[])
{
    // Initialize FUSE with specified operations

    // Filter argc and argv here and then pass it to fuse_main
    if (argc < 3)
    {
        printf("Usage: %s <mountpoint> <disk image>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    char *disk_path = argv[argc - 2];
    disk_fd = open(disk_path, O_RDWR);
    if (disk_fd == -1)
    {
        perror("Error opening disk");
        exit(EXIT_FAILURE);
    }
      // Remove the disk image path from the argument list passed to fuse_main
    // Note: we need to shift the mount point to where the disk image path was.
    argv[argc - 2] = argv[argc - 1];
    argc--;
    printf("argc: %d\n", argc);
    printf("argv[0]: %s\n", argv[0]);
    printf("argv[1]: %s\n", argv[1]);
    printf("argv[2]: %s\n", argv[2]);
    printf("argv[3]: %s\n", argv[3]);
    printf("argv[4]: %s\n", argv[4]);
    // Initialize your filesystem here if needed
    // ...

    return fuse_main(argc, argv, &ops, NULL);
}

/*
static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    //find inode for path
    struct wfs_inode* inode = find_inode(path);
    if (inode == NULL) {
        return -ENOENT; // No such file or directory
    }

    if (!S_ISREG(inode->mode)) {
        return -EISDIR; // Is a directory
    }

    if (offset >= inode->size) {
        return 0; // EOF
    }

    if (offset + size > inode->size) {
        size = inode->size - offset; // Read up to the end of the file
    }

    // Read the data from the inode
    memcpy(buf, ((char*)inode) + sizeof(struct wfs_inode) + offset, size);

    return size; // Return the number of bytes read
}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    //find inode for path
    struct wfs_inode* inode = find_inode(path);
    if (inode == NULL) {
        return -ENOENT; // No such file or directory
    }

    if (!S_ISREG(inode->mode)) {
        return -EISDIR; // Is a directory
    }

    if (offset + size > inode->size) {
        // Need to grow the file
        inode->size = offset + size;
    }

    // Write the data to the inode
    memcpy(((char*)inode) + sizeof(struct wfs_inode) + offset, buf, size);

    return size; // Return the number of bytes written
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    //find inode for path
    struct wfs_inode* inode = find_inode(path);
    if (inode == NULL) {
        return -ENOENT; // No such file or directory
    }

    if (!S_ISDIR(inode->mode)) {
        return -ENOTDIR; // Not a directory
    }

    // Casting inode to access data
    struct wfs_dentry* dentry = (struct wfs_dentry*)((char*)inode + sizeof(struct wfs_inode));

    for (int i = 0; i < inode->size / sizeof(struct wfs_dentry); ++i) {
        if (filler(buf, dentry[i].name, NULL, 0) != 0) {
            return -ENOMEM; // Out of memory
        }
    }

    return 0; // Return 0 on success
}

static int wfs_mknod(const char *path, mode_t mode, dev_t rdev) {
    //find inode for path
    struct wfs_inode* inode = find_inode(path);
    if (inode != NULL) {
        return -EEXIST; // File exists
    }

    //find parent inode for path
    char* path_copy = strdup(path); // Make a mutable copy of the path
    char* parent_path = dirname(path_copy);
    struct wfs_inode* parent_inode = find_inode(parent_path);
    free(path_copy);

    if (parent_inode == NULL) {
        return -ENOENT; // No such file or directory
    }

    if (!S_ISDIR(parent_inode->mode)) {
        return -ENOTDIR; // Not a directory
    }

    // Casting parent_inode to access data
    struct wfs_dentry* dentry = (struct wfs_dentry*)((char*)parent_inode + sizeof(struct wfs_inode));

    // Find an empty dentry
    int i;
    for (i = 0; i < parent_inode->size / sizeof(struct wfs_dentry); ++i) {
        if (dentry[i].inode_number == 0) {
            break;
        }
    }

    if (i == parent_inode->size / sizeof(struct wfs_dentry)) {
        return -ENOSPC; // No space left on device
    }

    // Create the new inode
    struct wfs_inode new_inode = {
        .inode_number = i + 1,
        .mode = mode,
        .uid = getuid(),
        .gid = getgid(),
        .size = 0,
        .links = 1
    };

    // Create the new dentry
    struct wfs_dentry new_dentry = {
        .inode_number = new_inode.inode_number
    };
    strncpy(new_dentry.name, basename(path), MAX_FILE_NAME_LEN);

    // Create the new log entry
    struct wfs_log_entry new_entry = {
        .inode = new_inode
    };

    // Write the new inode
    write_log_entry(new_entry);

    // Write the new dentry
    memcpy(((char*)parent_inode) + sizeof(struct wfs_inode) + i * sizeof(struct wfs_dentry), &new_dentry, sizeof(struct wfs_dentry));

    // Update the parent inode
    parent_inode->size += sizeof(struct wfs_dentry);

    return 0; // Return 0 on success
}

static int wfs_mkdir(const char *path, mode_t mode) {
    //find inode for path
    struct wfs_inode* inode = find_inode(path);
    if (inode != NULL) {
        return -EEXIST; // File exists
    }

    //find parent inode for path
    char* path_copy = strdup(path); // Make a mutable copy of the path
    char* parent_path = dirname(path_copy);
    struct wfs_inode* parent_inode = find_inode(parent_path);
    free(path_copy);

    if (parent_inode == NULL) {
        return -ENOENT; // No such file or directory
    }

    if (!S_ISDIR(parent_inode->mode)) {
        return -ENOTDIR; // Not a directory
    }

    // Casting parent_inode to access data
    struct wfs_dentry* dentry = (struct wfs_dentry*)((char*)parent_inode + sizeof(struct wfs_inode));

    // Find an empty dentry
    int i;
    for (i = 0; i < parent_inode->size / sizeof(struct wfs_dentry); ++i) {
        if (dentry[i].inode_number == 0) {
            break;
        }
    }

    if (i == parent_inode->size / sizeof(struct wfs_dentry)) {
        return -ENOSPC; // No space left on device
    }

    // Create the new inode
    struct wfs_inode new_inode = {
        .inode_number = i + 1,
        .mode = S_IFDIR,
        .uid = getuid(),
        .gid = getgid(),
        .size = 0,
        .links = 1
    };

    // Create the new dentry
    struct wfs_dentry new_dentry = {
        .inode_number = new_inode.inode_number
    };
    strncpy(new_dentry.name, basename(path), MAX_FILE_NAME_LEN);

    // Create the new log entry
    struct wfs_log_entry new_entry = {
        .inode = new_inode
    };

    // Write the new inode
    write_log_entry(new_entry);

    // Write the new dentry
    memcpy(((char*)parent_inode) + sizeof(struct wfs_inode) + i * sizeof(struct wfs_dentry), &new_dentry, sizeof(struct wfs_dentry));

    // Update the parent inode
    parent_inode->size;

    return 0; // Return 0 on success
}

static int wfs_unlink(const char *path) {
    //find inode for path
    struct wfs_inode* inode = find_inode(path);
    if (inode == NULL) {
        return -ENOENT; // No such file or directory
    }

    if (S_ISDIR(inode->mode)) {
        return -EISDIR; // Is a directory
    }

    //find parent inode for path
    char* path_copy = strdup(path); // Make a mutable copy of the path
    char* parent_path = dirname(path_copy);
    struct wfs_inode* parent_inode = find_inode(parent_path);
    free(path_copy);

    if (parent_inode == NULL) {
        return -ENOENT; // No such file or directory
    }

    if (!S_ISDIR(parent_inode->mode)) {
        return -ENOTDIR; // Not a directory
    }

    // Casting parent_inode to access data
    struct wfs_dentry* dentry = (struct wfs_dentry*)((char*)parent_inode + sizeof(struct wfs_inode));

    // Find the dentry for the file
    int i;
    for (i = 0; i < parent_inode->size / sizeof(struct wfs_dentry); ++i) {
        if (dentry[i].inode_number == inode->inode_number) {
            break;
        }
    }

    if (i == parent_inode->size / sizeof(struct wfs_dentry)) {
        return -ENOENT; // No such file or directory
    }

    // Delete the dentry
    memset(&dentry[i], 0, sizeof(struct wfs_dentry));

    // Delete the inode
    memset(inode, 0, sizeof(struct wfs_inode));

    // Update the parent inode
    parent_inode->size -= sizeof(struct wfs_dentry);

    return 0; // Return 0 on success
}

*/

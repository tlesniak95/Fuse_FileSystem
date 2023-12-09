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

struct wfs_inode *find_inode(const char *path)
{

    if (disk_fd == -1)
    {
        perror("Error opening filesystem image");
        return NULL;
    }

    // If the path is the root directory, handle it as a special case
    if (strcmp(path, "/") == 0)
    {
        struct wfs_inode *root_inode = malloc(sizeof(struct wfs_inode));
        if (root_inode == NULL)
        {
            perror("Error allocating memory for root inode");
            return NULL;
        }

        // The root inode is the first inode after the superblock
        off_t root_inode_offset = sizeof(struct wfs_sb);
        ssize_t read_size = pread(disk_fd, root_inode, sizeof(struct wfs_inode), root_inode_offset);
        if (read_size != sizeof(struct wfs_inode))
        {
            perror("Error reading root inode");
            free(root_inode);
            return NULL;
        }

        // If the root inode is marked as deleted, it's an error
        if (root_inode->deleted)
        {
            free(root_inode);
            return NULL;
        }

        return root_inode;
    }

    // Start after superblock
    off_t current_offset = sizeof(struct wfs_sb);

    char *path_copy = strdup(path);
    if (path_copy == NULL)
    {
        perror("Failed to duplicate path");
        return NULL;
    }
    char *token = strtok(path_copy, "/");
    struct wfs_inode *found_inode = NULL;
    struct wfs_log_entry *entry = NULL;
    struct wfs_inode *current_inode = NULL; // Most recent inode for each path component

    while (1)
    {
        free(entry); // Safe to call free on NULL
        entry = read_log_entry(disk_fd, current_offset);
        if (!entry)
        {
            // End of log or error
            break;
        }

        if (current_inode == NULL || entry->inode.inode_number == current_inode->inode_number)
        {
            free(found_inode);
            found_inode = malloc(sizeof(struct wfs_inode));
            if (!found_inode)
            {
                perror("Error allocating memory for inode");
                break; // Exit loop to handle cleanup
            }
            memcpy(found_inode, &entry->inode, sizeof(struct wfs_inode));
            if (entry->inode.deleted)
            {
                free(found_inode);
                found_inode = NULL;
                break; // Path component has been deleted
            }
        }
        ////////////////////////MAKING SMALL CHANGE HERE TO sizeof INODE
        current_offset += sizeof(struct wfs_inode) + entry->inode.size;

        if (found_inode && found_inode->mode == S_IFDIR && token)
        {
            struct wfs_dentry *dentries = (struct wfs_dentry *)(entry->data);
            size_t num_dentries = entry->inode.size / sizeof(struct wfs_dentry);
            int found = 0;
            for (size_t i = 0; i < num_dentries; ++i)
            {
                if (strcmp(dentries[i].name, token) == 0)
                {
                    token = strtok(NULL, "/");
                    current_inode = found_inode;
                    found = 1;
                    break;
                }
            }
            if (!found)
            {
                free(found_inode);
                found_inode = NULL;
                break; // Path component not found in current directory
            }
        }
        else if (found_inode && !S_ISDIR(found_inode->mode) && token == NULL)
        {
            // Last path component found and it's not a directory
            break;
        }
    }

    free(path_copy);
    free(entry); // Free the last read log entry

    return found_inode; // Could be NULL if path not found or an error occurred
}

/*
Return file attributes. The "stat" structure is described in detail in the stat(2) manual page.
For the given pathname, this should fill in the elements of the "stat" structure.
If a field is meaningless or semi-meaningless (e.g., st_ino) then it should be set to 0 or
given a "reasonable" value. This call is pretty much required for a usable filesystem.
*/
static int wfs_getattr(const char *path, struct stat *stbuf)
{
    memset(stbuf, 0, sizeof(struct stat)); // Clear the stat structure

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

static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    // Find the inode for the file
    struct wfs_inode *inode = find_inode(path);
    if (inode == NULL)
    {
        return -ENOENT; // No such file
    }

    // Ensure this is not a directory
    if (inode->mode == S_IFDIR)
    {
        free(inode);
        return -EISDIR; // Is a directory
    }

    // Start after the superblock
    off_t current_offset = sizeof(struct wfs_sb);
    struct wfs_log_entry *entry;
    struct wfs_log_entry *latest_entry = NULL;

    // Traverse the log to find the latest entry for the inode
    while ((entry = read_log_entry(disk_fd, current_offset)) != NULL)
    {
        if (entry->inode.inode_number == inode->inode_number && !entry->inode.deleted)
        {
            free(latest_entry); // Free any previously found entry
            latest_entry = entry;
        }
        else
        {
            free(entry); // Not the entry we're looking for
        }
        current_offset += sizeof(struct wfs_inode) + entry->inode.size;
    }

    if (latest_entry == NULL)
    {
        free(inode);
        return -ENOENT; // No entry found for this inode
    }

    // Calculate how much data to read
    size_t bytes_to_read = (latest_entry->inode.size - offset > size) ? size : latest_entry->inode.size - offset;

    // Copy the data from the latest log entry
    memcpy(buf, latest_entry->data + offset, bytes_to_read);

    // Clean up
    free(inode);
    free(latest_entry);

    // Return the number of bytes read
    return bytes_to_read;
}

static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    //.mknod      = wfs_mknod,
    //.mkdir      = wfs_mkdir,
    .read = wfs_read,
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

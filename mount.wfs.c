#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include "wfs.h"
#include <stdlib.h>
#include <unistd.h>
#include <libgen.h>

int disk_fd = -1;

unsigned int max_inode;
unsigned int * used_inodes;
struct wfs_sb sb;

struct wfs_log_entry *find_last_log_entry(int fd, unsigned int inode_number) {
    off_t current_offset = sizeof(struct wfs_sb); // Assuming the log starts after the superblock
    struct wfs_log_entry *latest_entry = NULL;
    size_t entry_size;

    while (current_offset < sb.head) {
        // Read the inode part of the log entry
        struct wfs_inode temp_inode;
        ssize_t read_size = pread(fd, &temp_inode, sizeof(struct wfs_inode), current_offset);
        if (read_size != sizeof(struct wfs_inode)) {
            perror("Error reading inode, helper func");
            // End of log or error
            break;
        }

        // Calculate the size of the log entry
        entry_size = sizeof(struct wfs_inode) + temp_inode.size;

        // Check if this log entry is for the inode we're looking for
        if (temp_inode.inode_number == inode_number && !temp_inode.deleted) {
            // Free the previous latest entry
            if (latest_entry) {
                free(latest_entry);
            }

            // Allocate memory and read the full log entry
            latest_entry = (struct wfs_log_entry *)malloc(entry_size);
            if (!latest_entry) {
                perror("Error allocating memory for log entry");
                break;
            }

            if (pread(fd, latest_entry, entry_size, current_offset) != entry_size) {
                perror("Error reading log entry");
                free(latest_entry);
                latest_entry = NULL;
                break;
            }
        }
        // Move to the next log entry
        current_offset += entry_size;
    }
    //Print current offset
    return latest_entry; // Return the latest entry found or NULL if none
}

// Function to read a log entry from the disk at a given offset
struct wfs_log_entry *read_log_entry(int fd, off_t offset)
{
    // Read the inode first to determine the size of the log entry
    struct wfs_inode inode;
    if (pread(fd, &inode, sizeof(struct wfs_inode), offset) != sizeof(struct wfs_inode))
    {
        perror("Error reading inode");
        // printf("Error reading inode\n");
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


unsigned int find_inode_number(const char *path) {
    if (disk_fd == -1) {
        perror("Error opening filesystem image");
        return -1;
    }

    // If the path is the root directory, handle it as a special case
    if (strcmp(path, "/") == 0) {
        return 0;
    }

    // Tokenize the path
    char *path_copy = strdup(path);
    if (path_copy == NULL) {
        perror("Failed to duplicate path");
        return -1;
    }

    char *token = strtok(path_copy, "/");
    unsigned int current_inode_number = 0;

    while (token != NULL) {
        struct wfs_log_entry *entry = find_last_log_entry(disk_fd, current_inode_number);
        if (entry == NULL) {
           // The entry doesn't exist
            free(path_copy);
            return -1; 
        }

        // Check if it's a regular file and matches the last component of the path
        if (S_ISREG(entry->inode.mode) && strcmp(token, path_copy) == 0) {
            unsigned int file_inode_number = entry->inode.inode_number;
            free(entry);
            free(path_copy);
            return file_inode_number;
        }

        // If it's a directory, iterate over the directory entries
        if (S_ISDIR(entry->inode.mode)) {
            struct wfs_dentry *dentries = (struct wfs_dentry *)(entry->data);
            size_t num_dentries = entry->inode.size / sizeof(struct wfs_dentry);
            unsigned int next_inode_number = -1;
            for (size_t i = 0; i < num_dentries; i++) {
                if (strcmp(dentries[i].name, token) == 0) {
                    next_inode_number = dentries[i].inode_number;
                    break;
                }
            }

            if (next_inode_number == -1) {
                // The next component of the path was not found in the current directory
                free(entry);
                free(path_copy);
                return -1;
            }

            // Move to the next component of the path
            current_inode_number = next_inode_number;
        } else {
            // Not a directory or a matching file
            free(entry);
            free(path_copy);
            return -1;
        }

        token = strtok(NULL, "/");
    }

    free(path_copy);
    return current_inode_number; // Return the inode number of the final path component
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

    unsigned int inode_number = find_inode_number(path);

    //Maybe change this later, since find_inode_number might return -1 for other errors as well. 
    if (inode_number == -1) {
        return -ENOENT; // No such file or directory
    }

    struct wfs_log_entry *entry = find_last_log_entry(disk_fd, inode_number);

    //Again, this might be wrong. 
    if (entry == NULL) {
        return -ENOENT; // No such file or directory
    }

    struct wfs_inode *inode = &(entry->inode);
    // //Print path and inode number
    // printf("path: %s\n", path);

    stbuf->st_mode = inode->mode;
    stbuf->st_nlink = inode->links;
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_size = inode->size;
    stbuf->st_mtime = inode->mtime;

    return 0; // Return 0 on success
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    // (void) offset; // Unused parameter
    // (void) fi;     // Unused parameter

    // Find the inode number for the given directory path
    unsigned int dir_inode_number = find_inode_number(path);
    if (dir_inode_number == -1) {
        // Directory not found
        return -ENOENT;
    }

    // Get the latest log entry for this directory inode
    struct wfs_log_entry *dir_entry = find_last_log_entry(disk_fd, dir_inode_number);
    if (dir_entry == NULL || !S_ISDIR(dir_entry->inode.mode)) {
        // Either the entry doesn't exist or it's not a directory
        //May not be the correct error
        return -ENOTDIR;
    }

    // Iterate over the directory entries stored in the log entry's data
    struct wfs_dentry *dentries = (struct wfs_dentry *)(dir_entry->data);
    size_t num_dentries = dir_entry->inode.size / sizeof(struct wfs_dentry);
    for (size_t i = 0; i < num_dentries; ++i) {
        // Call filler function to fill the buffer with directory entries
        // The filler function returns 1 when the buffer is full, 0 otherwise
        if (filler(buf, dentries[i].name, NULL, 0) != 0) {
            // Buffer is full, stop reading
            free(dir_entry);
            return 0;
        }
    }

    // Clean up
    free(dir_entry);
    return 0;
}


static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    // (void) fi; // Unused parameter

    // Find the inode number for the given file path
    unsigned int file_inode_number = find_inode_number(path);
    if (file_inode_number == -1) {
        // File not found
        return -ENOENT;
    }

    // Get the latest log entry for this file inode
    struct wfs_log_entry *file_entry = find_last_log_entry(disk_fd, file_inode_number);
    if (file_entry == NULL || !S_ISREG(file_entry->inode.mode)) {
        // Either the entry doesn't exist or it's not a regular file
        //May not be the correct error
        return -EISDIR; 
    }

    // Calculate the amount of data to read
    size_t data_size = file_entry->inode.size;
    size_t read_size = size;
    if (offset >= data_size) {
        // Offset is beyond the end of the file
        read_size = 0;
    } else if (offset + size > data_size) {
        // Adjust read_size so as not to read beyond the end of the file
        read_size = data_size - offset;
    }

    // Copy data from the log entry into the buffer
    memcpy(buf, file_entry->data + offset, read_size);

    // Clean up
    free(file_entry);

    // Return the number of bytes read
    return read_size;
}


static int wfs_mkdir(const char *path, mode_t mode) {
    printf("Debug: wfs_mknod, gets here!\n");
    // Check if the file already exists
    unsigned int inode_number = find_inode_number(path);
    if (inode_number != -1) {
        return -EEXIST; // File exists
    }

    // Extract the parent directory's path and name of the new file
    char *path_copy_dir = strdup(path); // Make a copy for dirname
    char *path_copy_base = strdup(path); // Make a copy for basename
    if (!path_copy_dir || !path_copy_base) {
        // Handle memory allocation failure
        free(path_copy_dir);
        free(path_copy_base);
        return -ENOMEM;
    }
    char *parent_path = dirname(path_copy_dir);
    char *base_name = basename(path_copy_base);
    printf("Debug: parent_path = %s\n", parent_path);
    printf("Debug: base_name = %s\n", base_name);
    printf("Debug: path = %s\n", path);
    // Find inode number for the parent directory
    unsigned int parent_inode_number = find_inode_number(parent_path);
    if (parent_inode_number == -1) {
        free(path_copy_dir);
        free(path_copy_base);
        return -ENOENT; // Parent directory doesn't exist
    }

    // Get the last log entry of the parent directory
    struct wfs_log_entry *parent_entry = find_last_log_entry(disk_fd, parent_inode_number);
    if (parent_entry == NULL || !S_ISDIR(parent_entry->inode.mode)) {
        free(path_copy_dir);
        free(path_copy_base);
        return -ENOTDIR; // Parent is not a directory
    }

    // Allocate memory for the new dentry and add it to the parent's data
    size_t new_data_size = parent_entry->inode.size + sizeof(struct wfs_dentry);
    char *new_data = malloc(new_data_size);
    if (new_data == NULL) {
        free(path_copy_dir);
        free(path_copy_base);
        free(parent_entry);
        return -ENOMEM; // Not enough memory
    }

    // Manually copy each existing dentry to new data
    struct wfs_dentry *old_dentries = (struct wfs_dentry *)(parent_entry->data);
    size_t num_old_dentries = parent_entry->inode.size / sizeof(struct wfs_dentry);
    for (size_t i = 0; i < num_old_dentries; ++i) {
        struct wfs_dentry *current_dentry = (struct wfs_dentry *)(new_data + i * sizeof(struct wfs_dentry));
        *current_dentry = old_dentries[i];
    }

    // Create and add the new dentry at the end
    struct wfs_dentry *new_dentry = (struct wfs_dentry *)(new_data + num_old_dentries * sizeof(struct wfs_dentry));
    
    unsigned int new_inode_number = -1;
    // Find the next available inode number
    for (int i = 1; i < max_inode + 100; i++) {
        if (used_inodes[i] == 0) {
            new_inode_number = i;
            used_inodes[i] = 1;
            break;
        }
    }
    if (new_inode_number == -1) {
        free(new_data);
        free(parent_path);
        return -ENOSPC;
    }
    new_dentry->inode_number = new_inode_number;
    strncpy(new_dentry->name, base_name, MAX_FILE_NAME_LEN - 1);
    new_dentry->name[MAX_FILE_NAME_LEN - 1] = '\0'; // Ensure null termination
    printf("Debug: new_dentry->inode_number = %ld\n", new_dentry->inode_number);
    printf("Debug: new_dentry->name = %s\n", new_dentry->name);
    // Create the new inode
    struct wfs_inode new_inode = {
        .inode_number = new_inode_number,
        .mode = S_IFDIR | mode, // Regular file with the specified mode
        .uid = getuid(),
        .gid = getgid(),
        .size = 0,
        .links = 1,
        .deleted = 0
    };

    // Create a new log entry for the file
    struct wfs_log_entry new_file_entry = {
        .inode = new_inode
        // .data field is not needed as it's a file with no content yet
    };

    // Append the new file log entry to the log
    printf("Debug: sb.head = %d\n", sb.head);
    printf("Debug: sizeof(new_file_entry) = %lu\n", sizeof(new_file_entry));
    //Debug, print sizeof root entry and sb head, and sb size
    printf("Debug: sizeof root entry = %lu\n", sizeof(struct wfs_log_entry));
    printf("Debug: sb size = %lu\n", sizeof(sb));
    off_t write_offset = sb.head;
    if (pwrite(disk_fd, &new_file_entry, sizeof(new_file_entry), write_offset) != sizeof(new_file_entry)) {
        free(new_data);
        free(parent_entry);
        free(path_copy_dir);
        free(path_copy_base);
        printf("Error in pwrite, child\n");
        return -EIO; // I/O error
    }
    sb.head += sizeof(new_file_entry);

    printf("Debug: sb.head = %d\n", sb.head);
    write_offset = sb.head;
    // Update parent directory's log entry with the new data
    // parent_entry->inode.size = new_data_size;
    //Print num of old dentries
    printf("Debug: num_old_dentries = %lu\n", num_old_dentries);
    printf("Debug: sizeof(struct wfs_dentry) = %lu\n", sizeof(struct wfs_dentry));
    printf("Debug: Number of bytes to write = %lu\n", sizeof(struct wfs_inode) + (num_old_dentries + 1 * sizeof(struct wfs_dentry)));

    // Copy the inode part of the parent entry
    struct wfs_inode updated_parent_inode = parent_entry->inode;
    updated_parent_inode.size = (num_old_dentries + 1) * sizeof(struct wfs_dentry);

    // Allocate memory for the updated parent entry
    size_t updated_entry_size = sizeof(struct wfs_inode) + updated_parent_inode.size;
    struct wfs_log_entry *updated_parent_entry = (struct wfs_log_entry *)malloc(updated_entry_size);
    if (!updated_parent_entry) {
        free(new_data);
        free(parent_entry);
        free(path_copy_dir);
        free(path_copy_base);
        return -ENOMEM;
    }

    // Set up the updated parent entry
    updated_parent_entry->inode = updated_parent_inode;
    memcpy(updated_parent_entry->data, new_data, updated_parent_inode.size);

        // Debug: Print the details of the updated parent entry before writing to disk
    printf("Debug: updated_parent_entry->inode.inode_number = %d\n", updated_parent_entry->inode.inode_number);
    printf("Debug: updated_parent_entry->inode.size = %d\n", updated_parent_entry->inode.size);

    // Assuming the updated parent data is of type struct wfs_dentry
    struct wfs_dentry *updated_dentries = (struct wfs_dentry *)(updated_parent_entry->data);
    for (size_t i = 0; i < updated_parent_inode.size / sizeof(struct wfs_dentry); ++i) {
        printf("Debug: Dentry %lu - inode number = %lu, name = %s\n", i, updated_dentries[i].inode_number, updated_dentries[i].name);
    }

    // Optionally, print the data in hexadecimal format
    printf("Debug: Updated Parent Data in hex = ");
    for (size_t i = 0; i < updated_parent_inode.size; ++i) {
        printf("%02x ", ((unsigned char*)updated_parent_entry->data)[i]);
    }
    printf("\n");
    // Write the updated parent entry to disk
    write_offset = sb.head; // Current head position
    if (pwrite(disk_fd, updated_parent_entry, updated_entry_size, write_offset) != updated_entry_size) {
        free(updated_parent_entry);
        free(new_data);
        free(parent_entry);
        free(path_copy_dir);
        free(path_copy_base);
        printf("Error in pwrite, new parent entry\n");
        return -EIO; // I/O error
    }
    sb.head += updated_entry_size; // Update head position
    

    //See if you can read the new file entries, by reading the log for their inodes
    struct wfs_log_entry *new_file_entry_read = find_last_log_entry(disk_fd, new_inode_number);
    if (new_file_entry_read == NULL) {
        printf("Error reading new file entry\n");
        return -EIO;
    }
    //Print log entry inode and data
    printf("Debug: new_file_entry_read->inode.inode_number = %d\n", new_file_entry_read->inode.inode_number);
    printf("Debug: new_file_entry_read->inode.mode = %d\n", new_file_entry_read->inode.mode);
    printf("Debug: new_file_entry_read->inode.uid = %d\n", new_file_entry_read->inode.uid);
    printf("Debug: new_file_entry_read->inode.gid = %d\n", new_file_entry_read->inode.gid);
    printf("Debug: new_file_entry_read->inode.size = %d\n", new_file_entry_read->inode.size);
    printf("Debug: new_file_entry_read->inode.links = %d\n", new_file_entry_read->inode.links);
    printf("Debug: new_file_entry_read->inode.deleted = %d\n", new_file_entry_read->inode.deleted);
    printf("Debug: new_file_entry_read->data = %s\n", new_file_entry_read->data);
    //Print offset posiiton of log entry

    //Print length of data

    // See if you can read the new parent entries, by reading the log for their inodes
    struct wfs_log_entry *new_parent_entry_read = find_last_log_entry(disk_fd, parent_inode_number);
    if (new_parent_entry_read == NULL) {
        printf("Error reading new parent entry\n");
        return -EIO;
    }

    //Print log entry inode and data
    printf("Debug: new_parent_entry_read->inode.inode_number = %d\n", new_parent_entry_read->inode.inode_number);
    printf("Debug: new_parent_entry_read->inode.mode = %d\n", new_parent_entry_read->inode.mode);
    printf("Debug: new_parent_entry_read->inode.uid = %d\n", new_parent_entry_read->inode.uid);
    printf("Debug: new_parent_entry_read->inode.gid = %d\n", new_parent_entry_read->inode.gid);
    printf("Debug: new_parent_entry_read->inode.size = %d\n", new_parent_entry_read->inode.size);
    printf("Debug: new_parent_entry_read->inode.links = %d\n", new_parent_entry_read->inode.links);
    printf("Debug: new_parent_entry_read->inode.deleted = %d\n", new_parent_entry_read->inode.deleted);
    printf("Debug: new_parent_entry_read->data = %s\n", new_parent_entry_read->data);


    //Debug, see if you can find the inode numbers of the new file and parent using find_inode_number
    printf("Debug: new file inode number = %d\n", find_inode_number(path));
    printf("Debug: new parent inode number = %d\n", find_inode_number(parent_path));

        // Assuming new_file_entry_read is a pointer to your struct wfs_log_entry
    printf("Debug: Inode size = %d\n", new_file_entry_read->inode.size);

    // Print the data in hexadecimal format
    printf("Debug: Data in hex = ");
    for (int i = 0; i < new_file_entry_read->inode.size; ++i) {
        printf("%02x ", ((unsigned char*)new_file_entry_read->data)[i]);
    }
    printf("\n");

    // Assuming new_parent_entry_read is a pointer to your struct wfs_log_entry
    printf("Debug: Parent Inode size = %d\n", new_parent_entry_read->inode.size);

    if (new_parent_entry_read->inode.size >= sizeof(struct wfs_dentry)) {
        struct wfs_dentry *dentry = (struct wfs_dentry *)(new_parent_entry_read->data);

        // Print the inode number of the dentry
        printf("Debug: Dentry inode number = %lu\n", dentry->inode_number);

        // Print the name of the dentry
        printf("Debug: Dentry name = %s\n", dentry->name);
    } else {
        printf("Debug: Not enough data for a complete dentry\n");
    }

    // Optionally, you can still print the data in hexadecimal format
    printf("Debug: Parent Data in hex = ");
    for (int i = 0; i < new_parent_entry_read->inode.size; ++i) {
        printf("%02x ", ((unsigned char*)new_parent_entry_read->data)[i]);
    }
    printf("\n");


    // Clean up
    free(new_data);
    free(parent_entry);
    free(path_copy_dir);
    free(path_copy_base);

    // Update the superblock with the new head position
    if (pwrite(disk_fd, &sb, sizeof(sb), 0) != sizeof(sb)) {
        perror("Error updating superblock");
        return -EIO; // I/O error
    }

    return 0; // Success
}

static int wfs_mknod(const char *path, mode_t mode, dev_t rdev) {
    // Check if the file already exists
    unsigned int inode_number = find_inode_number(path);
    if (inode_number != -1) {
        return -EEXIST; // File exists
    }

    // Extract the parent directory's path and name of the new file
    char *path_copy_dir = strdup(path); // Make a copy for dirname
    char *path_copy_base = strdup(path); // Make a copy for basename
    if (!path_copy_dir || !path_copy_base) {
        // Handle memory allocation failure
        free(path_copy_dir);
        free(path_copy_base);
        return -ENOMEM;
    }
    char *parent_path = dirname(path_copy_dir);
    char *base_name = basename(path_copy_base);
    // Find inode number for the parent directory
    unsigned int parent_inode_number = find_inode_number(parent_path);
    if (parent_inode_number == -1) {
        free(path_copy_dir);
        free(path_copy_base);
        return -ENOENT; // Parent directory doesn't exist
    }

    // Get the last log entry of the parent directory
    struct wfs_log_entry *parent_entry = find_last_log_entry(disk_fd, parent_inode_number);
    if (parent_entry == NULL || !S_ISDIR(parent_entry->inode.mode)) {
        free(path_copy_dir);
        free(path_copy_base);
        return -ENOTDIR; // Parent is not a directory
    }

    // Allocate memory for the new dentry and add it to the parent's data
    size_t new_data_size = parent_entry->inode.size + sizeof(struct wfs_dentry);
    char *new_data = malloc(new_data_size);
    if (new_data == NULL) {
        free(path_copy_dir);
        free(path_copy_base);
        free(parent_entry);
        return -ENOMEM; // Not enough memory
    }

    // Manually copy each existing dentry to new data
    struct wfs_dentry *old_dentries = (struct wfs_dentry *)(parent_entry->data);
    size_t num_old_dentries = parent_entry->inode.size / sizeof(struct wfs_dentry);
    for (size_t i = 0; i < num_old_dentries; ++i) {
        struct wfs_dentry *current_dentry = (struct wfs_dentry *)(new_data + i * sizeof(struct wfs_dentry));
        *current_dentry = old_dentries[i];
    }

    // Create and add the new dentry at the end
    struct wfs_dentry *new_dentry = (struct wfs_dentry *)(new_data + num_old_dentries * sizeof(struct wfs_dentry));
    
    unsigned int new_inode_number = -1;
    // Find the next available inode number
    for (int i = 1; i < max_inode + 100; i++) {
        if (used_inodes[i] == 0) {
            new_inode_number = i;
            used_inodes[i] = 1;
            break;
        }
    }
    if (new_inode_number == -1) {
        free(new_data);
        free(parent_path);
        return -ENOSPC;
    }
    new_dentry->inode_number = new_inode_number;
    strncpy(new_dentry->name, base_name, MAX_FILE_NAME_LEN - 1);
    new_dentry->name[MAX_FILE_NAME_LEN - 1] = '\0'; // Ensure null termination
    // Create the new inode
    struct wfs_inode new_inode = {
        .inode_number = new_inode_number,
        .mode = S_IFREG | mode, // Regular file with the specified mode
        .uid = getuid(),
        .gid = getgid(),
        .size = 0,
        .links = 1,
        .deleted = 0
    };

    // Create a new log entry for the file
    struct wfs_log_entry new_file_entry = {
        .inode = new_inode
        // .data field is not needed as it's a file with no content yet
    };

    off_t write_offset = sb.head;
    if (pwrite(disk_fd, &new_file_entry, sizeof(new_file_entry), write_offset) != sizeof(new_file_entry)) {
        free(new_data);
        free(parent_entry);
        free(path_copy_dir);
        free(path_copy_base);
        printf("Error in pwrite, child\n");
        return -EIO; // I/O error
    }
    sb.head += sizeof(new_file_entry);

    printf("Debug: sb.head = %d\n", sb.head);
    write_offset = sb.head;

    // Copy the inode part of the parent entry
    struct wfs_inode updated_parent_inode = parent_entry->inode;
    updated_parent_inode.size = (num_old_dentries + 1) * sizeof(struct wfs_dentry);

    // Allocate memory for the updated parent entry
    size_t updated_entry_size = sizeof(struct wfs_inode) + updated_parent_inode.size;
    struct wfs_log_entry *updated_parent_entry = (struct wfs_log_entry *)malloc(updated_entry_size);
    if (!updated_parent_entry) {
        free(new_data);
        free(parent_entry);
        free(path_copy_dir);
        free(path_copy_base);
        return -ENOMEM;
    }

    // Set up the updated parent entry
    updated_parent_entry->inode = updated_parent_inode;
    memcpy(updated_parent_entry->data, new_data, updated_parent_inode.size);

    // Write the updated parent entry to disk
    write_offset = sb.head; // Current head position
    if (pwrite(disk_fd, updated_parent_entry, updated_entry_size, write_offset) != updated_entry_size) {
        free(updated_parent_entry);
        free(new_data);
        free(parent_entry);
        free(path_copy_dir);
        free(path_copy_base);
        printf("Error in pwrite, new parent entry\n");
        return -EIO; // I/O error
    }
    sb.head += updated_entry_size; // Update head position

    // Clean up
    free(new_data);
    free(parent_entry);
    free(path_copy_dir);
    free(path_copy_base);

    // Update the superblock with the new head position
    if (pwrite(disk_fd, &sb, sizeof(sb), 0) != sizeof(sb)) {
        perror("Error updating superblock");
        return -EIO; // I/O error
    }

    return 0; // Success
}

static int wfs_unlink(const char *path) {
    return 0;
}


static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    .mknod      = wfs_mknod,
    .mkdir      = wfs_mkdir,
    .read	    = wfs_read,
    //.write      = wfs_write,
    .readdir	= wfs_readdir,
    .unlink    	= wfs_unlink,
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
    //Initialize the used_inodes array
    // Step 1: Find the maximum inode number
    off_t current_offset = sizeof(struct wfs_sb);
    struct wfs_log_entry *entry;
    max_inode = 0;
    // This approach may be wrong, could be better to use an array of available inode numbers
    while (current_offset < sb.head && ((entry = read_log_entry(disk_fd, current_offset)) != NULL)) {
        if (entry->inode.inode_number > max_inode) {
            max_inode = entry->inode.inode_number;
        }
        current_offset += sizeof(struct wfs_inode) + entry->inode.size;
        free(entry);
    }
    //Create array of sie max_inode + 1
    //Initialize all values to 0

    used_inodes = malloc(sizeof(unsigned int) * (max_inode + 100));
    for (int i = 0; i < max_inode + 1; i++) {
        used_inodes[i] = 0;
    }

    //For every non deleted node set the value to 1
    current_offset = sizeof(struct wfs_sb);
    while (current_offset < sb.head && ((entry = read_log_entry(disk_fd, current_offset)) != NULL)) {
        if (!entry->inode.deleted) {
            used_inodes[entry->inode.inode_number] = 1;
        }
        current_offset += sizeof(struct wfs_inode) + entry->inode.size;
        free(entry);
    }
      // Remove the disk image path from the argument list passed to fuse_main
    // Note: we need to shift the mount point to where the disk image path was.
    argv[argc - 2] = argv[argc - 1];
    argc--;

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

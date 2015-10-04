
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <time.h>
#include <sys/stat.h>
#include <openssl/md5.h>

#define E(x)   {int __tmp_e = (x); if(__tmp_e)return __tmp_e;}

#define LIST_CHUNK_SIZE  1048576
#define LIST_MAX_CHUNKS  65536

char **exclude;
int exclude_len;

struct list_s
{
    char *chunks[LIST_MAX_CHUNKS];
    struct entry_s *entry;
    int offset;
    int nchunk;
    int len;
    long size;
} file_list, dir_list, link_list;

struct entry_s
{
    int len;
    int perm;
    int gid;
    int uid;
    time_t time;
    char path;
};

int list_init(struct list_s *list)
{
    list->chunks[0] = malloc(LIST_CHUNK_SIZE);
    if(list->chunks[0] == NULL)
        return errno;

    list->nchunk = 0;
    list->offset = 0;
    list->len = 0;
    list->size = 0;
    list->entry = (struct entry_s*)list->chunks[0];

    return 0;
}

int list_add(struct list_s *list, char *path, struct entry_s *entry)
{
    if(list->nchunk >= LIST_MAX_CHUNKS)
        return ENOMEM;

    entry->len++; // Include null termination
    if(list->offset + entry->len + offsetof(struct entry_s, path) + 1 > LIST_CHUNK_SIZE)
    {
        list->nchunk++;
        if(list->nchunk >= LIST_MAX_CHUNKS)
            return ENOMEM;
        list->offset = 0;
        list->chunks[list->nchunk] = malloc(LIST_CHUNK_SIZE);
        list->entry = (struct entry_s*)list->chunks[list->nchunk];
        if(list->chunks[list->nchunk] == NULL)
        {
            list->nchunk--;
            return errno;
        }
    }

    memcpy(list->entry, entry, offsetof(struct entry_s, path));
    memcpy(&list->entry->path, path, entry->len);
    list->entry = (struct entry_s*)(((char*)list->entry) + entry->len + offsetof(struct entry_s, path));
    list->offset += entry->len + offsetof(struct entry_s, path);
    list->len++;
    *((char*)list->entry) = 0; // End of chunk

    return 0;
}

struct entry_s *list_next(struct list_s *list, int *chunk, struct entry_s *entry)
{
    if(chunk == NULL || *chunk > list->nchunk || entry == NULL || list == NULL)
        return NULL;

    entry = (struct entry_s*)(((char*)entry) + entry->len + offsetof(struct entry_s, path));
    if(*((char*)entry) == 0)
    {
        (*chunk)++;
        if(*chunk > list->nchunk)
            entry = NULL;
        else
            entry = (struct entry_s*)list->chunks[*chunk];
    }

    return entry;
}

void list_del(struct list_s *list)
{
    for(int i = 0; i <= list->nchunk; i++)
    {
        free(list->chunks[i]);
        list->chunks[i] = NULL;
    }
}

int list_dir(const char *root_name)
{
    DIR *dir;
    struct stat file_stat;

    dir = opendir(root_name);

    if(!dir)
        return errno;

    while(1)
    {
        struct dirent *dentry;
        struct entry_s entry;
        const char *name;
        int i;
        char path[PATH_MAX];

        dentry = readdir(dir);
        if(!dentry)
            break;

        name = dentry->d_name;
        if(!strcmp(name, "..") || !strcmp(name, "."))
            continue;

        if(root_name[0] == '/' && root_name[1] == '\0')
            entry.len = snprintf(path, PATH_MAX, "/%s", name);
        else if(root_name[0] == '.' && root_name[1] == '\0')
            entry.len = snprintf(path, PATH_MAX, "%s", name);
        else
            entry.len = snprintf(path, PATH_MAX, "%s/%s", root_name, name);

        if(lstat(path, &file_stat))
            return errno;

        if(entry.len >= PATH_MAX)
            return errno;

        entry.gid = file_stat.st_gid;
        entry.uid = file_stat.st_uid;
        entry.time = file_stat.st_mtim.tv_sec;

        entry.perm =  (((file_stat.st_mode & S_IXOTH)?1:0) |
                      (((file_stat.st_mode & S_IWOTH)?1:0) << 1) |
                      (((file_stat.st_mode & S_IROTH)?1:0) << 2)) +
                      ((((file_stat.st_mode & S_IXGRP)?1:0) |
                      (((file_stat.st_mode & S_IWGRP)?1:0) << 1) |
                      (((file_stat.st_mode & S_IRGRP)?1:0) << 2)) * 10) +
                      ((((file_stat.st_mode & S_IXUSR)?1:0) |
                      (((file_stat.st_mode & S_IWUSR)?1:0) << 1) |
                      (((file_stat.st_mode & S_IRUSR)?1:0) << 2)) * 100);

        if(S_ISLNK(file_stat.st_mode))
        {
            E(list_add(&link_list, path, &entry));
        }
        else if(S_ISDIR(file_stat.st_mode))
        {
            E(list_add(&dir_list, path, &entry));

            i = 0;
            if(exclude_len > 0)
                for(; i < exclude_len; i++)
                    if(!strcmp(exclude[i], path))
                        break;

            if(i == exclude_len)
                if(list_dir(path) == ENOMEM)
                    return ENOMEM;
        }
        else
        {
            E(list_add(&file_list, path, &entry));
        }
    }

    if(closedir(dir))
        return errno;

    return 0;
}

int main(int argc, char **argv)
{
    struct entry_s *entry;
    FILE *file;
    char path[PATH_MAX];
    int len, error, i, chunk;

    if(argc < 2)
    {
        printf("Usage: lnb_fileindexer SOURCE_PATH EXCLUDE_PATH1 EXCLUDE_PATH2 ...\n");
        fflush(stdout);
        return EINVAL;
    }

    if(argc > 2)
    {
        exclude = &argv[2];
        exclude_len = argc - 2;
    }

    if(chdir(argv[1]))
    {
        perror("Error");
        return EINVAL;
    }

    if(list_init(&dir_list))
    {
        fprintf(stderr, "Error: Out of memory\n");
        fflush(stderr);
        return ENOMEM;
    }

    if(list_init(&file_list))
    {
        list_del(&dir_list);
        fprintf(stderr, "Error: Out of memory\n");
        fflush(stderr);
        return ENOMEM;
    }

    if(list_init(&link_list))
    {
        list_del(&link_list);
        list_del(&dir_list);
        fprintf(stderr, "Error: Out of memory\n");
        fflush(stderr);
        return ENOMEM;
    }

    printf("Indexing files... ");
    fflush(stdout);
    if(list_dir("."))
    {
        list_del(&dir_list);
        list_del(&file_list);
        list_del(&link_list);
        fprintf(stderr, "Error: Out of memory\n");
        fflush(stderr);
        return ENOMEM;
    }

    printf("Done\n");
    printf("Listing directories... ");
    fflush(stdout);

    file = fopen("/tmp/lnb_fileindexer_dirs", "w");
    if(file == NULL)
    {
        error = errno;
        fprintf(stderr, "Error: Unable to open temp file\n");
        fflush(stderr);
        list_del(&dir_list);
        list_del(&file_list);
        list_del(&link_list);
        return error;
    }

    entry = (struct entry_s *)dir_list.chunks[0];
    chunk = 0;
    for(i = 0; i < dir_list.len; i++)
    {
        fprintf(file, "%s\t%d\t%d\t%d\n", &entry->path, entry->perm, entry->gid, entry->uid);
        entry = list_next(&dir_list, &chunk, entry);
    }
    fclose(file);

    printf("Done\n");
    printf("Listing files... ");
    fflush(stdout);
    file = fopen("/tmp/lnb_fileindexer_files", "w");
    if(file == NULL)
    {
        list_del(&dir_list);
        list_del(&file_list);
        list_del(&link_list);
        fprintf(stderr, "Error: Unable to open temp file\n");
        fflush(stderr);
        return EIO;
    }

    entry = (struct entry_s*)file_list.chunks[0];
    chunk = 0;
    for(i = 0; i < file_list.len; i++)
    {
        fprintf(file, "%s\t%d\t%d\t%d\t%s", &entry->path, entry->perm, entry->gid, entry->uid, ctime(&entry->time));
        entry = list_next(&file_list, &chunk, entry);
    }
    printf("Done\n");
    printf("Listing links... ");
    fflush(stdout);

    entry = (struct entry_s *)link_list.chunks[0];
    chunk = 0;
    for(i = 0; i < link_list.len; i++)
    {
        len = readlink(&entry->path, path, PATH_MAX);
        path[len] = '\0';
        fprintf(file, "%s\t%d\t%d\t%d\t%s\n", &entry->path, entry->perm, entry->gid, entry->uid, path);
        entry = list_next(&link_list, &chunk, entry);
    }

    fclose(file);
    list_del(&dir_list);
    list_del(&file_list);
    list_del(&link_list);
    printf("Done\n");
    fflush(stdout);

    return 0;
}

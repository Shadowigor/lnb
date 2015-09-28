
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>
#include <openssl/md5.h>

#define E(x)   {int __tmp_e = (x); if(__tmp_e)return __tmp_e;}
#define __STR(x) #x
#define STR(x) __STR(x)

#define LIST_CHUNK_SIZE  1048576
#define LIST_MAX_CHUNKS  65536
#define THREAD_COUNT     16
#define FILE_CHUNK_SIZE  134217728

char **exclude;
int exclude_len;

struct list_s
{
    char *chunks[LIST_MAX_CHUNKS];
    struct entry_s *entry;
    int offset;
    int nchunk;
    int current_chunk;
    int len;
    long size;
} file_list, dir_list, link_list;

struct entry_s
{
    int len;
    int perm;
    int gid;
    int uid;
    long size;
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
    list->current_chunk = 0;
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
    list->size += entry->size;
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

int list_del(struct list_s *list)
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
        else
            entry.len = snprintf(path, PATH_MAX, "%s/%s", root_name, name);

        if(lstat(path, &file_stat))
            return errno;

        if(entry.len >= PATH_MAX)
            return errno;

        entry.gid = file_stat.st_gid;
        entry.uid = file_stat.st_uid;
        entry.perm = file_stat.st_mode & ALLPERMS;
        entry.size = file_stat.st_size;

        if(S_ISLNK(file_stat.st_mode))
        {
            E(list_add(&link_list, path, &entry));
        }
        else if(S_ISDIR(file_stat.st_mode))
        {
            E(list_add(&dir_list, path, &entry));

            if(exclude_len > 0)
                for(i = 0; i < exclude_len; i++)
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

struct thread_arg
{
    struct entry_s *entry;
    int chunk;
    char *cs;
    int error;
    int cnt;
};

void *checksum_worker(void *varg)
{
    struct thread_arg *arg = (struct thread_arg*)varg;
    unsigned char *data;
    MD5_CTX md5_context;
    FILE *file;
    int len;

    data = malloc(FILE_CHUNK_SIZE);
    if(data == NULL)
    {
        arg->error = errno;
        return NULL;
    }

    for(int i = 0; i < arg->cnt; i++)
    {
        file = fopen(&arg->entry->path, "r");
        if(file == NULL)
        {
            arg->error = errno;
            return &arg->entry->path;
        }

        MD5_Init(&md5_context);
        while((len = fread(data, 1, FILE_CHUNK_SIZE, file)) != 0)
            MD5_Update(&md5_context, data, len);
        MD5_Final(arg->cs, &md5_context);
        list_next(&file_list, &arg->chunk, arg->entry);
        arg->cs += MD5_DIGEST_LENGTH;
        fclose(file);
    }

    free(data);

    return NULL;
}

int main(int argc, char **argv)
{
    struct thread_arg args[THREAD_COUNT];
    struct entry_s *entry;
    pthread_t threads[THREAD_COUNT];
    FILE *file;
    char *cs, *cs_current, *error_file, path[PATH_MAX];
    int pos, error, i, chunk;
    long size_per_th, size_next_th, size_current;

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
    if(list_dir(argv[1]))
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
    printf("Listing links... ");
    fflush(stdout);

    file = fopen("/tmp/lnb_fileindexer_link", "w");
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

    entry = (struct entry_s *)link_list.chunks[0];
    chunk = 0;
    for(i = 0; i < link_list.len; i++)
    {
        pos = readlink(&entry->path, path, PATH_MAX);
        path[pos] = '\0';
        fprintf(file, "%s\t%d\t%d\t%d\t%s\n", &entry->path, entry->perm, entry->gid, entry->uid, path);
        entry = list_next(&dir_list, &chunk, entry);
    }

    printf("Done\n");
    printf("Calculating checksums with " STR(THREAD_COUNT) " treads... ");
    fflush(stdout);
    cs = malloc(file_list.len * 16);
    if(cs == NULL)
    {
        list_del(&dir_list);
        list_del(&file_list);
        list_del(&link_list);
        fprintf(stderr, "Error: Out of memory\n");
        fflush(stderr);
        return ENOMEM;
    }

    pos = 0;
    entry = (struct entry_s*)file_list.chunks[0];
    chunk = 0;
    size_per_th = file_list.size / THREAD_COUNT;
    size_next_th = 0;
    size_current = 0;
    error = 0;
    cs_current = cs;
printf("\n");
    for(i = 0; i < THREAD_COUNT - 1; i++)
    {
        args[i].cnt = 0;
        args[i].error = 0;
        args[i].entry = entry;
        args[i].chunk = chunk;
        args[i].cs = cs_current;
        size_next_th += size_per_th;
        do
        {
            entry = list_next(&file_list, &chunk, entry);
            size_current += entry->size;
            args[i].cnt++;
            pos++;
            cs_current += MD5_DIGEST_LENGTH;
        } while(size_current < size_next_th);
        pthread_create(&threads[i], NULL, checksum_worker, &args[i]);
        printf("Thread %2d: %d\n", i, size_current - (size_next_th - size_per_th));
    }
    printf("Thread %2d: %d\n", THREAD_COUNT - 1, file_list.len - size_current);

    args[THREAD_COUNT - 1].cnt = file_list.len - pos;
    args[THREAD_COUNT - 1].entry = entry;
    args[THREAD_COUNT - 1].chunk = chunk;
    args[THREAD_COUNT - 1].cs = cs_current;
    args[THREAD_COUNT - 1].error = 0;
    pthread_create(&threads[THREAD_COUNT - 1], NULL, checksum_worker, &args[THREAD_COUNT - 1]);

    for(i = 0; i < THREAD_COUNT; i++)
    {
            pthread_join(threads[i], (void**)&error_file);
            error |= args[i].error;
    }

    if(error)
    {
        if(error_file == NULL)
            fprintf(stderr, "Error: Out of memory\n");
        else
            fprintf(stderr, "Error: Cannot access all files (e.g. %s)\n", error_file);
        fflush(stderr);
        list_del(&dir_list);
        list_del(&file_list);
        list_del(&link_list);
        return EACCES;
    }

    printf("Done\n");
    fflush(stdout);

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
    cs_current = cs;
    for(i = 0; i < file_list.len; i++)
    {
        fprintf(file, "%s\t%d\t%d\t%d\t", &entry->path, entry->perm, entry->gid, entry->uid);
        for(int j = 0; j < MD5_DIGEST_LENGTH; j++)
        {
            fprintf(file, "%02hhX", *cs_current);
            cs_current++;
        }
        fprintf(file, "\n");
        entry = list_next(&file_list, &chunk, entry);
    }
    fclose(file);
    free(cs);
    list_del(&dir_list);
    list_del(&file_list);
    list_del(&link_list);
    printf("Done\n");
    fflush(stdout);

    return 0;
}

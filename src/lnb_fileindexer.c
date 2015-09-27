
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

#define E(x) {int __tmp_e = (x); if(__tmp_e)return __tmp_e;}

#define LIST_CHUNK_SIZE  1048576
#define LIST_MAX_CHUNKS  1024
#define LIST_MAX_ENTRIES 16777216
#define THREAD_COUNT     8
#define FILES_PER_THREAD 1000
#define FILE_CHUNK_SIZE  1048576

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
} file_list, dir_list;

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
        return ENOMEM;

    list->nchunk = 0;
    list->offset = 0;
    list->len = 0;
    list->size = 0;
    list->current_chunk = 0;
    list->entry = (struct entry_s*)list->chunks[0];
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
            return ENOMEM;
        }
    }

    memcpy(list->entry, entry, offsetof(struct entry_s, path));
    memcpy(&list->entry->path, path, entry->len);
    list->entry += entry->len + offsetof(struct entry_s, path);
    list->offset += entry->len + offsetof(struct entry_s, path);
    list->len++;
    list->size += entry->size;
    *((char*)list->entry) = 0; // End of chunk

    return 0;
}

struct entry_s *list_next(struct list_s *list)
{
    struct entry_s *entry;

    if(list->current_chunk > list->nchunk)
        return NULL;

    entry = list->entry;
    list->entry += entry->len + offsetof(struct entry_s, path);
    if(*((char*)list->entry) == 0)
    {
        list->current_chunk++;
        if(list->current_chunk > list->nchunk)
            list->entry = NULL;
        else
            list->entry = (struct entry_s*)list->chunks[list->current_chunk];
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
        return ENOENT;

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

        E(stat(path, &file_stat));

        if(entry.len >= PATH_MAX)
            return ENOENT;

        entry.gid = file_stat.st_gid;
        entry.uid = file_stat.st_uid;
        entry.perm = file_stat.st_mode & ALLPERMS;
        entry.size = file_stat.st_size;

        if(S_ISDIR(file_stat.st_mode) && !S_ISLNK(file_stat.st_mode))
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

    E(closedir(dir));

    return 0;
}

struct thread_arg
{
    struct entry_s *entry;
    char *cs;
    int error;
    int cnt;
};

void *checksum_worker(void *varg)
{
    struct thread_arg arg = *((struct thread_arg*)varg);
    unsigned char data[FILE_CHUNK_SIZE];
    MD5_CTX md5_context;
    FILE *file;
    int len;

    for(int i = 0; i < arg.cnt; i++)
    {
        file = fopen(&arg.entry->path, "r");
        if(file == NULL)
        {
            arg.error = EACCES;
            return NULL;
        }

        MD5_Init(&md5_context);
        while((len = fread(data, 1, FILE_CHUNK_SIZE, file)) != 0)
            MD5_Update(&md5_context, data, len);
        MD5_Final(arg.cs, &md5_context);
    }

    arg.error = 0;

    return NULL;
}

int main(int argc, char **argv)
{
    FILE *file;
    int i;

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

    if(!list_init(&dir_list))
    {
        fprintf(stderr, "Error: Out of memory\n");
        fflush(stderr);
        return ENOMEM;
    }

    if(!list_init(&file_list))
    {
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
        fprintf(stderr, "Error: Out of memory\n");
        fflush(stderr);
        return ENOMEM;
    }
    printf("Done\n");
    fflush(stdout);

    printf("Listing directories... ");
    fflush(stdout);
    file = fopen("/tmp/lnb_fileindexer_dirs", "w");
    if(file == NULL)
    {
        fprintf(stderr, "Error: Unable to open temp file\n");
        fflush(stderr);
        return EIO;
    }

    struct entry_s *entry;
    dir_list.current_chunk = 0;
    dir_list.entry = (struct entry_s *)dir_list.chunks[0];
    for(i = 0; i < dir_list.len; i++)
    {
        entry = list_next(&dir_list);
        fprintf(file, "%s\t%d\t%d\t%d\n", &entry->path, entry->perm, entry->gid, entry->uid);
    }

    fclose(file);
    printf("Done\n");
    fflush(stdout);

    pthread_t threads[THREAD_COUNT];
    struct thread_arg args[THREAD_COUNT];
    char *cs, *cs_current, pos;
    int error;
    long size_per_th, size_next_th, size_current;

    printf("Calculating checksums... ");
    fflush(stdout);
    cs = malloc(file_list.len * 16);
    if(cs == NULL)
    {
        list_del(&dir_list);
        list_del(&file_list);
        fprintf(stderr, "Error: Out of memory\n");
        fflush(stderr);
        return ENOMEM;
    }

    pos = 0;
    file_list.entry = (struct entry_s*)file_list.chunks[0];
    file_list.current_chunk = 0;
    size_per_th = file_list.size / THREAD_COUNT;
    entry = file_list.entry;
    size_next_th = 0;
    size_current = 0;
    error = 0;
    cs_current = cs;

    for(i = 0; i < THREAD_COUNT - 1; i++)
    {
        args[i].cnt = 0;
        args[i].error = 0;
        args[i].entry = entry;
        args[i].cs = cs_current;
        size_next_th += size_per_th;
        do
        {
            entry = list_next(&file_list);
            size_current += entry->size;
            args[i].cnt++;
            pos++;
            cs_current += MD5_DIGEST_LENGTH;
        } while(size_current < size_next_th);
        pthread_create(&threads[i], NULL, checksum_worker, &args[i]);
    }
    args[THREAD_COUNT - 1].cnt = file_list.len - pos;
    args[i].entry = entry;
    args[i].cs = cs_current;
    args[THREAD_COUNT].error = 0;
    pthread_create(&threads[i], NULL, checksum_worker, &args[THREAD_COUNT - 1]);

    for(i = 0; i < THREAD_COUNT; i++)
    {
            pthread_join(hreads[i], NULL);
            error |= args[i].error;
    }

    if(error)
    {
        fprintf(stderr, "Error: Cannot access all files");
        fflush(stderr);
        return EACCES;
    }

    printf("Done");
    fflush(stdout);

    printf("Listing files... ");
    fflush(stdout);
    file = fopen("/tmp/lnb_fileindexer_files", "w");
    if(file == NULL)
    {
        fprintf(stderr, "Error: Unable to open temp file");
        fflush(stderr);
        return EIO;
    }

    file_list.entry = (struct entry_s*)file_list.chunks[0];
    file_list.current_chunk = 0;
    entry = file_list.entry;
    cs_current = cs;
    for(i = 0; i < file_list.len; i++)
    {
        fprintf(file, "%s\t%d\t%d\t%d\t", &entry->path, entry->perm, entry->gid, entry->uid);
        for(int j = 0; j < MD5_DIGEST_LENGTH; j++)
        {
            fprintf(file, "%X", cs_current);
            cs_current++;
        }
        fprintf(file, "\n");
    }
    fclose(file);
    printf("Done");
    fflush(stdout);

    return 0;
}

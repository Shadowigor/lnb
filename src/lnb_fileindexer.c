
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/md5.h>

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
    char *p[LIST_MAX_ENTRIES];
    char cs[LIST_MAX_ENTRIES][MD5_DIGEST_LENGTH];
    int perm[LIST_MAX_ENTRIES];
    int uid[LIST_MAX_ENTRIES];
    int gid[LIST_MAX_ENTRIES];
    char *chunks[LIST_MAX_CHUNKS];
    int offset;
    int nchunk;
    int len;
} file_list, dir_list;

int list_init(struct list_s *list)
{
    list->chunks[0] = malloc(LIST_CHUNK_SIZE);
    if(list->chunks[0] == NULL)
        return 1;

    list->nchunk = 0;
    list->offset = 0;
    list->len = 0;
}

int list_add(struct list_s *list, const char *path, int len)
{
    if(list->nchunk >= LIST_MAX_CHUNKS)
        return 1;

    if(list->len >= LIST_MAX_ENTRIES)
        return 3;

    len++; // Include null termination
    if(list->offset + len > LIST_CHUNK_SIZE)
    {
        list->nchunk++;
        if(list->nchunk >= LIST_MAX_CHUNKS)
            return 1;
        list->offset = 0;
        list->chunks[list->nchunk] = malloc(LIST_CHUNK_SIZE);
        if(list->chunks[list->nchunk] == NULL)
        {
            list->nchunk--;
            return 2;
        }
    }
    list->p[list->len] = list->chunks[list->nchunk] + list->offset;
    strncpy(list->p[list->len], path, len);
    list->len++;
    list->offset += len;
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
        return 1;

    while(1)
    {
        struct dirent *entry;
        const char *d_name;
        int path_length, i;
        char path[PATH_MAX];

        entry = readdir(dir);
        if(!entry)
            break;

        d_name = entry->d_name;
        if(!strcmp(d_name, "..") || !strcmp(d_name, "."))
            continue;

        if(root_name[0] == '/' && root_name[1] == '\0')
            path_length = snprintf(path, PATH_MAX, "/%s", d_name);
        else
            path_length = snprintf(path, PATH_MAX, "%s/%s", root_name, d_name);

        if(stat(path, &file_stat) < 0)
            return 1;

        if(path_length >= PATH_MAX)
            return 1;

        if(S_ISDIR(file_stat.st_mode) && !S_ISLNK(file_stat.st_mode))
        {
            if(!list_add(&dir_list, path, path_length))
                return 1;

            dir_list.perm[i] = file_stat.st_mode & ALLPERMS;
            dir_list.uid[i] = file_stat.st_uid;
            dir_list.gid[i] = file_stat.st_gid;

            if(exclude_len > 0)
                for(i = 0; i < exclude_len; i++)
                    if(!strcmp(exclude[i], path))
                        break;

            if(i == exclude_len)
                list_dir(path);
        }
        else
        {
            if(!list_add(&file_list, path, path_length))
                return 1;

            file_list.perm[i] = file_stat.st_mode & ALLPERMS;
            file_list.uid[i] = file_stat.st_uid;
            file_list.gid[i] = file_stat.st_gid;
        }
    }

    if(closedir(dir))
        return 1;

    return 0;
}

struct thread_arg
{
    char **paths;
    char (*cs)[LIST_MAX_ENTRIES][MD5_DIGEST_LENGTH];
    int is_running;
    int len;
};

void *checksum_worker(void *varg)
{
    struct thread_arg arg = *((struct thread_arg*)varg);
    unsigned char data[FILE_CHUNK_SIZE];
    MD5_CTX md5_context;
    FILE *file;
    int len;

    for(int i = 0; i < arg.len; i++)
    {
        file = fopen(arg.paths[i], "r");
        if(file == NULL)
            continue;

        MD5_Init(&md5_context);
        while((len = fread(data, 1, FILE_CHUNK_SIZE, file)) != 0)
            MD5_Update(&md5_context, data, len);
        MD5_Final((*arg.cs)[i], &md5_context);
    }

    pthread_exit(NULL);
}

int main(int argc, char **argv)
{
    FILE *file;
    int i;

    if(argc < 2)
    {
        printf("Usage: lnb_fileindexer SOURCE_PATH EXCLUDE_PATH1 EXCLUDE_PATH2 ...");
        return 1;
    }

    if(argc > 2)
    {
        exclude = &argv[2];
        exclude_len = argc - 2;
    }

    list_init(&dir_list);
    list_init(&file_list);

    if(!list_dir(argv[1]))
    {
        fprintf(stderr, "Error: Out of memory");
        return 1;
    }

    file = fopen("/tmp/lnb_fileindexer_dirs", "w");
    if(file == NULL)
    {
        fprintf(stderr, "Error: Unable to open temp file");
        return 1;
    }

    for(i = 0; i < dir_list.len; i++)
        fprintf(file, "%s\n", dir_list.p[i]);

    fclose(file);

    pthread_t threads[THREAD_COUNT];
    struct thread_arg args[THREAD_COUNT];
    char **data, (*cs)[LIST_MAX_ENTRIES][MD5_DIGEST_LENGTH], **data_end;

    data = file_list.p;
    cs = &file_list.cs;
    data_end = data + file_list.len;
    for(i = 0; i < THREAD_COUNT; i++)
        args[i].is_running = 0;

    while(data < data_end)
    {
        for(i = 0; i < THREAD_COUNT; i++)
            if(!args[i].is_running)
                break;

        if(i < THREAD_COUNT)
        {
            if(data + FILES_PER_THREAD >= data_end)
                args[i].len = (int)(data_end - data);
            else
                args[i].len = FILES_PER_THREAD;

            args[i].paths = data;
            args[i].cs = cs;
            args[i].is_running = 1;
            pthread_create(&threads[i], NULL, checksum_worker, &args[i]);
            data += args[i].len;
            cs += args[i].len;
        }
        usleep(10);
    }

    for(i = 0; i < THREAD_COUNT; i++)
        if(args[i].is_running)
            pthread_join(threads[i], NULL);


    file = fopen("/tmp/lnb_fileindexer_files", "w");
    if(file == NULL)
    {
        fprintf(stderr, "Error: Unable to open temp file");
        return 1;
    }

    for(i = 0; i < file_list.len; i++)
    {
        fprintf(file, "%s\t%n\t%n\t%n\t", file_list.p[i], file_list.perm[i], file_list.uid[i], file_list.gid[i]);
        for(int j = 0; j < MD5_DIGEST_LENGTH; j++)
            fprintf(file, "%X", file_list.cs[i][j]);
        fprintf(file, "\n");
    }
    fclose(file);
}

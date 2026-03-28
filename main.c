#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

struct mini_unionfs_state
{
    char lower[256];
    char upper[256];
};

/* PATH RESOLUTION */
int resolve_path(char *resolved, const char *path,
                 struct mini_unionfs_state *state)
{

    char upper_path[512];
    char lower_path[512];

    sprintf(upper_path, "%s%s", state->upper, path);
    sprintf(lower_path, "%s%s", state->lower, path);

    if (access(upper_path, F_OK) == 0)
    {
        strcpy(resolved, upper_path);
        return 0;
    }

    if (access(lower_path, F_OK) == 0)
    {
        strcpy(resolved, lower_path);
        return 0;
    }

    return -ENOENT;
}

/* TEST GETATTR */
void test_getattr(const char *path, struct mini_unionfs_state *state)
{
    char resolved[512];
    struct stat st;

    if (resolve_path(resolved, path, state) != 0)
    {
        printf("File not found: %s\n", path);
        return;
    }

    if (stat(resolved, &st) == -1)
    {
        perror("stat");
        return;
    }

    printf("GETATTR: %s\n", resolved);
    printf("Size: %lld bytes\n", (long long)st.st_size);
}

/* TEST READDIR */
void test_readdir(struct mini_unionfs_state *state)
{
    DIR *dp;
    struct dirent *de;

    printf("\nMerged Directory Listing:\n");

    // Upper directory
    dp = opendir(state->upper);
    if (dp)
    {
        while ((de = readdir(dp)) != NULL)
        {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            printf("[UPPER] %s\n", de->d_name);
        }
        closedir(dp);
    }

    // Lower directory
    dp = opendir(state->lower);
    if (dp)
    {
        while ((de = readdir(dp)) != NULL)
        {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            printf("[LOWER] %s\n", de->d_name);
        }
        closedir(dp);
    }
}

/* MAIN (SIMULATION MODE) */
int main()
{

    struct mini_unionfs_state state;
    strcpy(state.lower, "lower");
    strcpy(state.upper, "upper");

    printf("---- UNIONFS SIMULATION ----\n");

    char resolved[512];

    // Test path resolution
    if (resolve_path(resolved, "/file.txt", &state) == 0)
    {
        printf("Resolved Path: %s\n", resolved);
    }
    else
    {
        printf("File not found\n");
    }

    // Test getattr
    test_getattr("/file.txt", &state);

    // Test readdir
    test_readdir(&state);

    return 0;
}
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <assert.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

// cc: histogram of length 2848. tells you how many times a cluster is refered to by the fat

int cc[4096] = {0}; //from 2^12
static int id = 0;

#define FIND_FILE 0
#define FIND_DIR 1

void get_name(char *fullname, struct direntry *dirent) 
{
    char name[9];
    char extension[4];
    int i;

    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);

    /* names are space padded - remove the padding */
    for (i = 8; i > 0; i--) 
    {
    if (name[i] == ' ') 
        name[i] = '\0';
    else 
        break;
    }

    /* extensions aren't normally space padded - but remove the
       padding anyway if it's there */
    for (i = 3; i > 0; i--) 
    {
    if (extension[i] == ' ') 
        extension[i] = '\0';
    else 
        break;
    }
    fullname[0]='\0';
    strcat(fullname, name);

    /* append the extension if it's not a directory */
    if ((dirent->deAttributes & ATTR_DIRECTORY) == 0) 
    {
    strcat(fullname, ".");
    strcat(fullname, extension);
    }
}


struct direntry* find_file(char *infilename, uint16_t cluster,
               int find_mode,
               uint8_t *image_buf, struct bpb33* bpb)
{
    char buf[MAXPATHLEN];
    char *seek_name, *next_name;
    int d;
    struct direntry *dirent;
    uint16_t dir_cluster;
    char fullname[13];

    /* find the first dirent in this directory */
    dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

    /* first we need to split the file name we're looking for into the
       first part of the path, and the remainder.  We hunt through the
       current directory for the first part.  If there's a remainder,
       and what we find is a directory, then we recurse, and search
       that directory for the remainder */

    strncpy(buf, infilename, MAXPATHLEN);
    seek_name = buf;

    /* trim leading slashes */
    while (*seek_name == '/' || *seek_name == '\\') 
    {
    seek_name++;
    }

    /* search for any more slashes - if so, it's a dirname */
    next_name = seek_name;
    while (1) 
    {
    if (*next_name == '/' || *next_name == '\\') 
    {
        *next_name = '\0';
        next_name ++;
        break;
    }
    if (*next_name == '\0') 
    {
        /* end of name - no slashes found */
        next_name = NULL;
        if (find_mode == FIND_DIR) 
        {
        return dirent;
        }
        break;
    }
    next_name++;
    }

    while (1) 
    {
    /* hunt a cluster for the relevant dirent.  If we reach the
       end of the cluster, we'll need to go to the next cluster
       for this directory */
    for (d = 0; 
         d < bpb->bpbBytesPerSec * bpb->bpbSecPerClust; 
         d += sizeof(struct direntry)) 
    {
        if (dirent->deName[0] == SLOT_EMPTY) 
        {
        /* we failed to find the file */
        return NULL;
        }

        if (dirent->deName[0] == SLOT_DELETED) 
        {
        /* skip over a deleted file */
        dirent++;
        continue;
        }

        get_name(fullname, dirent);
        if (strcmp(fullname, seek_name)==0) 
        {
        /* found it! */
        if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) 
        {
            /* it's a directory */
            if (next_name == NULL) 
            {
            fprintf(stderr, "Cannot copy out a directory\n");
            exit(1);
            }
            dir_cluster = getushort(dirent->deStartCluster);
            return find_file(next_name, dir_cluster, 
                     find_mode, image_buf, bpb);
        } 
        else if ((dirent->deAttributes & ATTR_VOLUME) != 0) 
        {
            /* it's a volume */
            fprintf(stderr, "Cannot copy out a volume\n");
            exit(1);
        } 
        else 
        {
            /* assume it's a file */
            return dirent;
        }
        }
        dirent++;
    }

    /* we've reached the end of the cluster for this directory.
       Where's the next cluster? */
    if (cluster == 0) 
    {
        // root dir is special
        dirent++;
    } 
    else 
    {
        cluster = get_fat_entry(cluster, image_buf, bpb);
        dirent = (struct direntry*)cluster_to_addr(cluster, 
                               image_buf, bpb);
    }
    }
}

uint16_t copy_in_file(FILE* fd, uint8_t *image_buf, struct bpb33* bpb, 
              uint32_t *size)
{
    uint32_t clust_size, total_clusters, i;
    uint8_t *buf;
    size_t bytes;
    uint16_t start_cluster = 0;
    uint16_t prev_cluster = 0;
    
    clust_size = bpb->bpbSecPerClust * bpb->bpbBytesPerSec;
    total_clusters = bpb->bpbSectors / bpb->bpbSecPerClust;
    buf = malloc(clust_size);
    while(1) 
    {
    /* read a block of data, and store it */
    bytes = fread(buf, 1, clust_size, fd);
    if (bytes > 0) {
        *size += bytes;

        /* find a free cluster */
        for (i = 2; i < total_clusters; i++) 
        {
        if (get_fat_entry(i, image_buf, bpb) == CLUST_FREE) 
        {
            break;
        }
        }

        if (i == total_clusters) 
        {
        /* oops - we ran out of disk space */
        fprintf(stderr, "No more space in filesystem\n");
        /* we should clean up here, rather than just exit */ 
        exit(1);
        }

        /* remember the first cluster, as we need to store this in
           the dirent */
        if (start_cluster == 0) 
        {
        start_cluster = i;
        } 
        else 
        {
        /* link the previous cluster to this one in the FAT */
        assert(prev_cluster != 0);
        set_fat_entry(prev_cluster, i, image_buf, bpb);
        }

        /* make sure we've recorded this cluster as used */
        set_fat_entry(i, FAT12_MASK&CLUST_EOFS, image_buf, bpb);

        /* copy the data into the cluster */
        memcpy(cluster_to_addr(i, image_buf, bpb), buf, clust_size);
    }

    if (bytes < clust_size) 
    {
        /* We didn't real a full cluster, so we either got a read
           error, or reached end of file.  We exit anyway */
        break;
    }
    prev_cluster = i;
    }

    free(buf);
    return start_cluster;
}

void write_dirent(struct direntry *dirent, char *filename, 
          uint16_t start_cluster, uint32_t size)
{
    char *p, *p2;
    char *uppername;
    int len, i;

    /* clean out anything old that used to be here */
    memset(dirent, 0, sizeof(struct direntry));

    /* extract just the filename part */
    uppername = strdup(filename);
    p2 = uppername;
    for (i = 0; i < strlen(filename); i++) 
    {
        if (p2[i] == '/' || p2[i] == '\\') 
        {
            uppername = p2+i+1;
        }
    }

    /* convert filename to upper case */
    for (i = 0; i < strlen(uppername); i++) 
    {
    uppername[i] = toupper(uppername[i]);
    }

    /* set the file name and extension */
    memset(dirent->deName, ' ', 8);
    p = strchr(uppername, '.');
    memcpy(dirent->deExtension, "___", 3);
    if (p == NULL) 
    {
    fprintf(stderr, "No filename extension given - defaulting to .___\n");
    }
    else 
    {
    *p = '\0';
    p++;
    len = strlen(p);
    if (len > 3) len = 3;
    memcpy(dirent->deExtension, p, len);
    }

    if (strlen(uppername)>8) 
    {
    uppername[8]='\0';
    }
    memcpy(dirent->deName, uppername, strlen(uppername));
    free(p2);

    /* set the attributes and file size */
    dirent->deAttributes = ATTR_NORMAL;
    putushort(dirent->deStartCluster, start_cluster);
    putulong(dirent->deFileSize, size);

    /* could also set time and date here if we really
       cared... */
}

void create_dirent(struct direntry *dirent, char *filename, 
           uint16_t start_cluster, uint32_t size,
           uint8_t *image_buf, struct bpb33* bpb)
{
    while (1) 
    {
    if (dirent->deName[0] == SLOT_EMPTY) 
    {
        /* we found an empty slot at the end of the directory */
        write_dirent(dirent, filename, start_cluster, size);
        dirent++;

        /* make sure the next dirent is set to be empty, just in
           case it wasn't before */
        memset((uint8_t*)dirent, 0, sizeof(struct direntry));
        dirent->deName[0] = SLOT_EMPTY;
        return;
    }

    if (dirent->deName[0] == SLOT_DELETED) 
    {
        /* we found a deleted entry - we can just overwrite it */
        write_dirent(dirent, filename, start_cluster, size);
        return;
    }
    dirent++;
    }
}

void copyin(char *infilename, char* outfilename,
        uint8_t *image_buf, struct bpb33* bpb)
{
    struct direntry *dirent = (void*)1;
    FILE *fd;
    uint16_t start_cluster;
    uint32_t size = 0;

    assert(strncmp("a:", outfilename, 2)==0);
    outfilename+=2;

    /* check that the file doesn't already exist */
    dirent = find_file(outfilename, 0, FIND_FILE, image_buf, bpb);
    if (dirent != NULL) 
    {
    fprintf(stderr, "File %s already exists\n", outfilename);
    exit(1);
    }

    /* find the dirent of the directory to put the file in */
    dirent = find_file(outfilename, 0, FIND_DIR, image_buf, bpb);
    if (dirent == NULL) 
    {
    fprintf(stderr, "Directory does not exists in the disk image\n");
    exit(1);
    }

    /* open the real file for reading */
    fd = fopen(infilename, "r");
    if (fd == NULL) 
    {
    fprintf(stderr, "Can't open file %s to copy data in\n",
        infilename);
    exit(1);
    }

    /* do the actual copy in*/
    start_cluster = copy_in_file(fd, image_buf, bpb, &size);

    /* create the directory entry */
    create_dirent(dirent, outfilename, start_cluster, size, image_buf, bpb);
    
    fclose(fd);
}

void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}

void fix_orphan(int orphan, struct direntry *orphan_dirent, uint8_t *image_buf, struct bpb33* bpb){
    char* orphan_file = ("found1.dat");

    int size_of_orphan_cluster = traverse_fat(orphan_dirent, image_buf, bpb);

    uint16_t cluster = 0;
    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);
    write_dirent(dirent, orphan_file, orphan, size_of_orphan_cluster);
    id++;
    cc[orphan] = id;
    return;
}

void find_orphan(struct direntry *dirent, uint8_t *image_buf, struct bpb33* bpb){
    int count=0;
    for (int i = 2 ; i<2848; i++){ // for each cluster
        if (cc[i]==0){
            if (get_fat_entry(i, image_buf, bpb)!=0){

                printf("orphan found: %i\n", i);
                fix_orphan(i, dirent, image_buf, bpb);
                count++;

            }
        }
    }
}

int traverse_fat(struct direntry *dirent, uint8_t *image_buf, struct bpb33* bpb){
    id++;
    uint16_t start_cluster = getushort(dirent->deStartCluster);
    uint32_t size = getulong(dirent->deFileSize);
    size = ((size+511)/512);
    uint16_t fat_entry = get_fat_entry(start_cluster, image_buf, bpb);
    uint16_t prev_fat = fat_entry;
    int count = 1;

    while (!(is_end_of_file(fat_entry))){
        //this bad entry thing might not even work!!!
        if (fat_entry == (FAT12_MASK & CLUST_BAD)){
            printf("Defect in cluster %i\n", count);
            cc[fat_entry] = -1;
        }
        if (count >= size){
            uint16_t tmp = get_fat_entry(fat_entry, image_buf, bpb);

            //unlink the previous entry with this entry.
            if (count==size){
                printf("Found something tooo big!\n");
                fflush(stdout);
                set_fat_entry(prev_fat, FAT12_MASK&CLUST_EOFS, image_buf, bpb);
                assert(get_fat_entry(prev_fat,image_buf,bpb)==(FAT12_MASK&CLUST_EOFS));
            }

            //set the current cluster to free
            set_fat_entry(fat_entry, FAT12_MASK&CLUST_FREE, image_buf, bpb);
            assert(get_fat_entry(fat_entry, image_buf, bpb)==0);
            printf("Fixed!\n");
            //prev_fat = fat_entry;
            fat_entry = tmp;
            count++;

        }
        else {
            //go to next entry
            cc[fat_entry] = id;
            prev_fat = fat_entry;
            fat_entry = get_fat_entry(fat_entry, image_buf, bpb);
            count ++;
        }
    }
    //count++;
    if (size>count){
        printf("Metadata is bigger than cluster data -- adjusting metadata\n");
        putulong(dirent->deFileSize, count*512);
        printf("Should be fixed now\n");

    }
    return count;
}

uint16_t build_cc(struct direntry *dirent, struct bpb33 *bpb, uint8_t *image_buf){
    uint16_t followclust = 0;

    int i;
    char name[9];
    char extension[4];
    uint32_t size;
    uint16_t file_cluster;
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);

    if (name[0] == SLOT_EMPTY || ((uint8_t)name[0]) == SLOT_DELETED || ((uint8_t)name[0]) == 0x2E )
    {
        return followclust;
    }

    /* names are space padded - remove the spaces */
    for (i = 8; i > 0; i--) 
    {
        if (name[i] == ' ') 
            name[i] = '\0';
        else 
            break;
    }

    /* remove the spaces from extensions */
    for (i = 3; i > 0; i--) 
    {
        if (extension[i] == ' ') 
            extension[i] = '\0';
        else 
            break;
    }
    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN){
    }
    
    else if ((dirent->deAttributes & ATTR_VOLUME) != 0) {
        printf("Volume: %s\n", name);
    } 

    else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) 
        {
        // don't deal with hidden directories; MacOS makes these
        // for trash directories and such; just ignore them.
        if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN)
            {
                file_cluster = getushort(dirent->deStartCluster);
                followclust = file_cluster;
            }
    }

    else 
    {
        /*
         * a "regular" file entry
         * print attributes, size, starting cluster, etc.
         */
    int ro = (dirent->deAttributes & ATTR_READONLY) == ATTR_READONLY;
    int hidden = (dirent->deAttributes & ATTR_HIDDEN) == ATTR_HIDDEN;
    int sys = (dirent->deAttributes & ATTR_SYSTEM) == ATTR_SYSTEM;
    int arch = (dirent->deAttributes & ATTR_ARCHIVE) == ATTR_ARCHIVE;

    size = getulong(dirent->deFileSize);
    int count = traverse_fat(dirent, image_buf, bpb);
    if (count!=((size+511)/512)){
        printf("\t%s.%s (%u bytes %d clusters) (starting cluster %d) %c%c%c%c\n", 
               name, extension, size, ((size + 512 - 1) / 512),  getushort(dirent->deStartCluster),
               ro?'r':' ', 
                   hidden?'h':' ', 
                   sys?'s':' ', 
                   arch?'a':' ');
        printf ("********Discrepancy: %i metadata clusters != %i FAT clusters\n", (size + 511)/512, count);
    }
    }
    return followclust;
}

void follow_dir(uint16_t cluster, uint8_t *image_buf, struct bpb33* bpb)
{
    while (is_valid_cluster(cluster, bpb))
    {
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
    for ( ; i < numDirEntries; i++)
    {
            
            uint16_t followclust = build_cc(dirent, bpb, image_buf);
            if (followclust)
                follow_dir(followclust, image_buf, bpb);
            dirent++;
    }

    cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}

void traverse_root(uint8_t *image_buf, struct bpb33* bpb)
{
    uint16_t cluster = 0;

    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);
    int i = 0;
    for ( ; i < bpb->bpbRootDirEnts; i++)
    {
        //printf("traverse root\n");
        uint16_t followclust = build_cc(dirent, bpb, image_buf);
        if (is_valid_cluster(followclust, bpb))
            follow_dir(followclust, image_buf, bpb);

        dirent++;
    }
    
}

int main(int argc, char** argv) {
    uint8_t *image_buf;
    int fd;
    struct bpb33* bpb;
    if (argc < 2) {
    usage(argv[0]);
    }

    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);
    // your code should start here...

    // 1) Traverse Root - For each Directory Entry:
    //      a) Traverse FAT Entries to make sure directory size matches FAT linked-list_length.
    //      b) Fix any discrepencies, and print which ones they are.
    // 2) Traverse Through Data Area:
    //      a) Make sure everything has a proper labeling
    traverse_root(image_buf, bpb);

    // set up
    uint16_t cluster = 0;
    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);
    //find_orphan(dirent, image_buf, bpb);

    printf("Done!\n");
    fflush(stdout);
    //print_cc();

    unmmap_file(image_buf, &fd);
    return 0;
}
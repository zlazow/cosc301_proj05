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

int cc[4096] = {0}; //from 2^12
static int id = 0;

void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}

void fix_orphan(int start_cluster, struct direntry *dirent, uint8_t *image_buf, struct bpb33* bpb){

    // make a new file in root
    // creates if not there, otherwise opens it

    FILE *fp = fopen("dumpster.dat", "w+");
    printf("start cluster:%i\n", start_cluster);

    uint16_t cluster = get_fat_entry(start_cluster, image_buf, bpb);

    if (fp == NULL) {
        fprintf(stderr, "Can't open dumpster file\n");
        exit(1);
    }
    
    fprintf(fp, "start cluster: %c\n", cluster);

    id++;
    cc[start_cluster] = id;
    
}

void find_orphan(struct direntry *dirent, uint8_t *image_buf, struct bpb33* bpb){
    int count=0;
    for (int i = 2 ; i<2848; i++){
        if (cc[i]<=0){
            if (get_fat_entry(i, image_buf, bpb)!=0){
                //printf("%i\n", get_fat_entry(i, image_buf, bpb));
                fix_orphan(i, dirent, image_buf, bpb);
                count++;
            }
        }
    }
    //printf("%i\n", count);
}

int traverse_fat(struct direntry *dirent, uint8_t *image_buf, struct bpb33* bpb){
    id++;
    uint16_t start_cluster = getushort(dirent->deStartCluster);
    uint32_t size = getulong(dirent->deFileSize);
    size = ((size+511)/512);
    uint16_t fat_entry = get_fat_entry(start_cluster, image_buf, bpb);
    uint16_t prev_fat = fat_entry;
    int count = 1;

    // check case where file size is just 1
/*
    if (is_end_of_file(fat_entry)){
        if (size>count){
            printf("Size>count, we should probably do something here...\n");
        }
        return count;
    }
*/

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
/*
    int size = (getulong(dirent->deFileSize) + 511)/512;
    if (count!=size){
        if ((dirent->deAttributes & ATTR_VOLUME) == 0){
            if ((dirent->deAttributes & ATTR_DIRECTORY) == 0){
                printf("****Discrepancy: %i clusters\n\n", count);
            }
        }
    }
    return 1;
*/
}

uint16_t build_cc(struct direntry *dirent, struct bpb33 *bpb, uint8_t *image_buf)
{
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
    //if (count!=((size+511)/512)){
        printf("\t%s.%s (%u bytes %d clusters) (starting cluster %d) %c%c%c%c\n", 
               name, extension, size, ((size + 512 - 1) / 512),  getushort(dirent->deStartCluster),
               ro?'r':' ', 
                   hidden?'h':' ', 
                   sys?'s':' ', 
                   arch?'a':' ');
        printf ("********Discrepancy: %i metadata clusters != %i FAT clusters\n", (size + 511)/512, count);
    //}
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
        uint16_t followclust = build_cc(dirent, bpb, image_buf);
        if (is_valid_cluster(followclust, bpb))
            follow_dir(followclust, image_buf, bpb);

        dirent++;
    }
    find_orphan(dirent, image_buf, bpb);
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
    printf("Done!\n");
    fflush(stdout);
    //print_cc();

    unmmap_file(image_buf, &fd);
    return 0;
}
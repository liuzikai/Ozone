#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <string.h>
#include <sys/param.h>
#include <libgen.h>

#include <aos/aos.h>
#include <aos/cache.h>
#include <fs/fs.h>
#include <fs/fat32.h>
#include <fs/list.h>
#include <aos/deferred.h>

#include "fs_internal.h"

//tracking data for current cluster; used for getting free blocks
#define FREE_CLUSTERS_SCANNED_BLOCKS 2

#define FIRST_SECTOR_OF_CLUSTER(n) ((n-2) * manager->SecPerClus) + manager->FirstDataSector
#define FAT_SECTOR(n) manager->RsvdSecCnt + (n * 4 / manager->BytsPerSec)
#define FAT_OFFSET(n) (n*4) % manager->BytsPerSec
#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

#define CHECK_ERR(f, msg) ({\
            err = f;\
            if(err_is_fail(err)) {\
                DEBUG_ERR(err, msg);\
                return err;\
            }\
})

#define CHECK_ERR_PUSH(f, push) ({\
            err = f;\
            if(err_is_fail(err)) {\
                return err_push(err, push);\
            }\
})

void set_sd(struct sdhc_s *sdh) {
    if(manager)
        manager->sd = sdh;
}

static void shortname_to_name(char *shortname, char **retname) {
    char *name = calloc(1, 12);
    int i = 0, k = 0;
    while(i < strlen(shortname) && shortname[i] != 0x20) name[k++] = shortname[i++];
    i = 8;
    if(shortname[i] != 0x20) name[k++] = '.';
    while(i < strlen(shortname) && shortname[i] != 0x20) name[k++] = shortname[i++];
    name[k] = '\0'; 
    *retname = name;
}

static void name_to_shortname(char *name, char *shortname) {
    memset(shortname, ' ', 11);
    if(name[0] == '.') {
        shortname[0] = '.';
        if(name[1] != '\0' && name[1] == '.')
            shortname[1] = '.';
        return;
    }
    uint32_t lenf, lenl;
    char *split = strchr(name, '.');
    
    if (split == NULL) {
        lenl = strnlen(name, 8);
        lenf = 0;
    } 
    else {
        lenl = split - name;
        split++;
        lenf = strnlen(split, 3);
    }
    for (int i = 0; i < lenl; i++) {
        shortname[i] = toupper(name[i]);
    }
    for (int i = 0; i < lenf; i++) {
        shortname[8+i] = toupper(split[i]);
    }
}

static bool strisalnum(char *name, int len) {
    for(int i=0;i<len;i++)
        if(!isalnum(name[i]))
            return false;
    
    return true;
}
static bool valid_shortname(char *name) {
    int len = strlen(name);
    if(len > 12 || len == 0)
        return false;
    if(name[0] == '.' || isdigit(name[0]))
        return false;
    char *dot_pos = strchr(name, '.');
    if(dot_pos == NULL) {
        if(len > 8 || !strisalnum(name, len))
            return false;
    }
    else {
        int lenfirst = dot_pos - name;
        if(lenfirst > 8)
            return false;
        int lenext = len - (dot_pos - name) - 1;
        if(lenext > 3)
            return false;
        if(!strisalnum(name, lenfirst) && !strisalnum(dot_pos+1, lenext))
            return false;
    }    
    return true;
}

static errval_t get_no_cache_frame(int size, lpaddr_t *paddr, lpaddr_t *vaddr, struct capref *retframe) {
    errval_t err;

    struct capref frame;

    err = frame_alloc(&frame, size, NULL);
    if(err_is_fail(err))
        return err;
    
    struct frame_identity f_id;
    err = cap_identify_mappable(frame, &f_id);
    if(err_is_fail(err))
        return err;
    *paddr = f_id.base;

    err = paging_map_frame_attr(get_current_paging_state(), (void **)vaddr, SDHC_BLOCK_SIZE, frame, VREGION_FLAGS_READ_WRITE_NOCACHE);
    if(err_is_fail(err))
        return err;
    
    *retframe = frame;
    return SYS_ERR_OK;
}

// //Read logical sector <sector> and return a pointer to the info
// static errval_t sd_read_sector(int sector, void *data)__attribute__((optimize ("Os")));

static errval_t sd_read_sector(int sector, void *data) {
    errval_t err;

    lpaddr_t paddr, vaddr;
    struct capref frame;

    CHECK_ERR(get_no_cache_frame(SDHC_BLOCK_SIZE, &paddr, &vaddr, &frame), "");
    
    CHECK_ERR_PUSH(sdhc_read_block(manager->sd, sector, paddr), FS_ERR_BLOCK_READ);

    // DEBUG_PRINTF("DATA OF SECTOR %d\n", sector);
    //I DO NOT KNOW WHY, BUT WITHOUT THESE LINES THE CODE BREAKS!!!!!
    //DEBUG_PRINTF("%d", *(uint8_t *)vaddr);
    //DEBUG_PRINTF("");
    // print_block((uint8_t *) vaddr);
	barrelfish_usleep(25000);

    memcpy(data, (void *)vaddr, SDHC_BLOCK_SIZE);
    
    CHECK_ERR(cap_destroy(frame), "");

    // DEBUG_PRINTF("READING WORKS\n");

    return SYS_ERR_OK;
}

//Write data to logical sector
static errval_t sd_write_sector(int sector, void *data) {
    errval_t err;

    lpaddr_t paddr, vaddr;
    struct capref frame;
    CHECK_ERR(get_no_cache_frame(SDHC_BLOCK_SIZE, &paddr, &vaddr, &frame), "");

    memcpy((void *)vaddr, data, SDHC_BLOCK_SIZE);

    CHECK_ERR_PUSH(sdhc_write_block(manager->sd, sector, paddr), FS_ERR_BLOCK_WRITE);

	barrelfish_usleep(25000);

    CHECK_ERR(cap_destroy(frame), "");

    return SYS_ERR_OK;
}

static void check_set_bpb_metadata(uint8_t *bpb) {
    assert(bpb[510] == 0x55);
    assert(bpb[511] == 0xAA);
    assert((bpb[0] == 0xEB && bpb[2] == 0x90) || bpb[0] == 0xE9);

    //grab all the metadata
    manager->BytsPerSec = *(uint16_t *)(bpb + BPB_BytsPerSec);
    manager->SecPerClus = *(bpb + BPB_SecPerClus);
    manager->RsvdSecCnt = *(uint16_t *)(bpb + BPB_RsvdSecCnt);
    manager->RootEntCnt = *(uint16_t *)(bpb + BPB_RootEntCnt);
    manager->RootClus = *(uint32_t *)(bpb + BPB_RootClus);
    manager->TotSec32 = *(uint32_t *)(bpb + BPB_TotSec32);
    manager->FATSz32 = *(uint32_t *)(bpb + BPB_FATSz32);
    manager->NumFATs = *(bpb + BPB_NumFATs);

    assert(manager->RootEntCnt == 0);

    //calculate the first data sector
    manager->FirstDataSector = manager->RsvdSecCnt + (manager->NumFATs * manager->FATSz32);
    //calculat the sector of the root cluster
    manager->RootSector = FIRST_SECTOR_OF_CLUSTER(manager->RootClus);
    //calculate total number of clusters in the volume
    manager->TotalClusters = manager->TotSec32/manager->SecPerClus;
    //calculate blocks per sector
    manager->BlocksPerSec = manager->BytsPerSec/SDHC_BLOCK_SIZE;

    //create and set root directory
    root_directory = malloc(sizeof(struct fat32_dirent));
    root_directory->Attr = ATTR_DIRECTORY;
    root_directory->FstCluster = manager->RootClus;
    root_directory->name = manager->mount;
    root_directory->parent = NULL;
    root_directory->size = -1;
    root_directory->is_dir = true;
    // DEBUG_PRINTF("BytsPerSec : %d\n", BytsPerSec);
    // DEBUG_PRINTF("SecPerClus : %d\n", SecPerClus);
    // DEBUG_PRINTF("RsvdSecCnt : %d\n", RsvdSecCnt);
    // DEBUG_PRINTF("RootEntCnt : %d\n", RootEntCnt);
    // DEBUG_PRINTF("RootClus : %d\n", RootClus);
    // DEBUG_PRINTF("TotSec32 : %d\n", TotSec32);
    // DEBUG_PRINTF("TotalClusters : %d\n", TotalClusters);
    // DEBUG_PRINTF("FATSz32 : %d\n", FATSz32);
    // DEBUG_PRINTF("NumFATs : %d\n", NumFATs);
    // DEBUG_PRINTF("FirstDataSector : %d\n", FirstDataSector);
    // DEBUG_PRINTF("RootSector : %d\n", RootSector);
    // DEBUG_PRINTF("RootFATSector : %d\n", FAT_SECTOR(RootClus));
    // DEBUG_PRINTF("RootFATOffset : %d\n", FAT_OFFSET(RootClus));
}

static errval_t refill_free_clusters(void) {
    errval_t err;
    if(manager->FreeClustersToCheckFrom == manager->TotalClusters)
        return FS_ERR_NO_FREE_BLOCKS;

    uint8_t FAT_block[SDHC_BLOCK_SIZE];

    //read first sector as offset may not be zero
    CHECK_ERR(sd_read_sector(FAT_SECTOR(DATA_CLUSTER_START), FAT_block), "FAT sector read failed");

    //iterate through free clusters
    int blocks = FREE_CLUSTERS_SCANNED_BLOCKS - 1;
    while((manager->FreeClustersToCheckFrom != manager->TotalClusters)) {
        int FAT_offset = FAT_OFFSET(manager->FreeClustersToCheckFrom);

        //if we are the start of a new FAT sector, read it
        if(FAT_offset == 0) {
            CHECK_ERR(sd_read_sector(FAT_SECTOR(manager->FreeClustersToCheckFrom), FAT_block), "FAT sector read failed");
            blocks--;
        }
        //grab entry from offset into block
        FAT_Entry entry = *(FAT_Entry *)(FAT_block + FAT_offset);
        //if free cluster, insert into free list
        if((entry & CLUSTER_FREE_MASK) == CLUSTER_FREE) {
            push_back(manager->free_clusters, manager->FreeClustersToCheckFrom);
        }
//I DO NOT KNOW WHY, BUT WITHOUT THESE LINES THE CODE BREAKS!!!!!
        if(!blocks && FAT_offset == 508)
            break;

        manager->FreeClustersToCheckFrom++;
    }

    return SYS_ERR_OK;
}

static errval_t initialize_free_clusters(void) {
    errval_t err;
    list_init((struct free_cluster_list **)&(manager->free_clusters));
    
    manager->FreeClustersToCheckFrom = DATA_CLUSTER_START;

    CHECK_ERR(refill_free_clusters(), "Failed to refill clusters");

    return SYS_ERR_OK;
}

static errval_t get_next_cluster(int cluster, int *next_cluster) {
    errval_t err;
    uint8_t FAT_Sector[SDHC_BLOCK_SIZE];

    CHECK_ERR(sd_read_sector(FAT_SECTOR(cluster), FAT_Sector), "failed to read FAT");
    FAT_Entry *entry = (FAT_Entry *) (FAT_Sector + FAT_OFFSET(cluster));
    *next_cluster = *entry;

    return SYS_ERR_OK;
}

static errval_t get_last_cluster(int cluster, int *last_cluster) {
    errval_t err;

    while(cluster != CLUSTER_FREE && cluster != CLUSTER_EOC) {
        CHECK_ERR(get_next_cluster(cluster, &cluster), "");
    }

    *last_cluster = cluster;
    return SYS_ERR_OK;
}

static errval_t allocate_cluster(int *retclus) {
    errval_t err;
    if(manager->free_clusters->size == 0) {
        CHECK_ERR_PUSH(refill_free_clusters(), FS_ERR_DISK_FULL);
    }

    *retclus = pop_front(manager->free_clusters);
    assert(*retclus >= 2);

    return SYS_ERR_OK;
}

static errval_t sector_from_cluster_offset(int cluster, int offset, int *retsector, int *retoffset) {
    if((cluster == CLUSTER_FREE) || (cluster == CLUSTER_EOC))
        return FS_ERR_INDEX_BOUNDS;
    
    errval_t err;
    
    if(offset > manager->BytsPerSec * manager->SecPerClus) {
        offset -= manager->BytsPerSec * manager->SecPerClus;
        int next_cluster;
        CHECK_ERR(get_next_cluster(cluster, &next_cluster), "");
        return sector_from_cluster_offset(next_cluster, offset, retsector, retoffset);
    }

    *retsector = FIRST_SECTOR_OF_CLUSTER(cluster) + (offset/manager->BytsPerSec);
    *retoffset = offset % manager->BytsPerSec;

    return SYS_ERR_OK;
}

//given a 32 byte directory entry, extracts info out of it
//TODO : get file times
static void parse_directory_entry(uint8_t *dir, struct fat32_dirent *parent, int sector, int offset, struct fat32_dirent **retent) {

    struct fat32_dirent *dirent = malloc(sizeof(struct fat32_dirent));
    
    char shortname[11];
    memcpy(shortname, dir, 11);
    shortname_to_name(shortname, &dirent->name);

    dirent->Attr = *(dir + DIR_ATTR);

    if(dirent->Attr == ATTR_DIRECTORY)
        dirent->is_dir = true;
    else
        dirent->is_dir = false;

    uint16_t cluster_high = *(uint16_t *) (dir + DIR_FST_CLUSTER_HIGH), cluster_low = *(uint16_t *) (dir + DIR_FST_CLUSTER_LOW);
    dirent->FstCluster = (cluster_high << 16) + cluster_low;

    dirent->parent = parent;
    dirent->size = *(uint32_t *) (dir + DIR_FILE_SIZE);

    dirent->sector = sector;
    dirent->sector_offset = offset;

    *retent = dirent;
}

//Marshall a dirent into a 32 byte FAT32 buffer
//TODO : handle file times
static void marshall_directory_entry(struct fat32_dirent *dir, uint8_t *buff) {
    memset(buff, 0, DIR_SIZE);
    
    char shortname[11];
    name_to_shortname(dir->name, shortname);

    memcpy(buff, shortname, DIR_NAME_SZ);

    memcpy(buff + DIR_ATTR, &dir->Attr, 1);

    uint16_t fst_cluster_high = dir->FstCluster >> 16;
    uint16_t fst_cluster_low = dir->FstCluster & (0b1111111111111111);
    memcpy(buff + DIR_FST_CLUSTER_HIGH, &fst_cluster_high, 2);
    memcpy(buff + DIR_FST_CLUSTER_LOW, &fst_cluster_low, 2);

    memcpy(buff + DIR_FILE_SIZE, &dir->size, 4);
}

static void free_dirent(struct fat32_dirent *dir, bool recursive) {
    if(dir->size == -1)
        return;
    if(dir == NULL)
        return;
    if(dir->name != NULL)
        free(dir->name);
    if(recursive)
        free_dirent(dir->parent, recursive);
    free(dir);
}

//if find_empty is set, finds the first empty dirent block
//if find_empty, retsector and retoffset mandatory, else not mandatory
//retcluster returns the cluster at which this was found (is important when you want to extend the cluster)
static errval_t find_in_directory(struct fat32_dirent *dir, const char *name, bool find_empty, struct fat32_dirent **retdir, int *retsector, int *retoffset, int *retcluster) {
    errval_t err;
    int cluster = dir->FstCluster;

    // DEBUG_PRINTF("We are trying to find %s in %s\n", name, dir->name);
    // DEBUG_PRINTF("We are opening cluster %d\n", cluster);
    if((cluster & CLUSTER_FREE_MASK) == CLUSTER_FREE) {
        if(retcluster)
            *retcluster = CLUSTER_FREE;
        return FS_ERR_NOTFOUND;
    }

    while(cluster != CLUSTER_EOC) {
        if(cluster == CLUSTER_BAD)
            return FS_ERR_BAD_CLUSTER;

        int start_sector = FIRST_SECTOR_OF_CLUSTER(dir->FstCluster);
        for(int sector = 0; sector < manager->SecPerClus; sector++) {
            uint8_t sector_data[SDHC_BLOCK_SIZE];
            CHECK_ERR(sd_read_sector(start_sector + sector, sector_data), "bad sd read");
            for(int i = 0; i < SDHC_BLOCK_SIZE; i+=32) {
                if(find_empty) {
                    if(sector_data[i] == DIR_ALL_FREE || sector_data[i] == DIR_FREE) {
                        *retsector = start_sector + sector;
                        *retoffset = i;
                        if(retcluster != NULL)
                            *retcluster = cluster;
                        return SYS_ERR_OK;
                    }
                }
                else {
                    if(sector_data[i] == DIR_ALL_FREE) {
                        return FS_ERR_NOTFOUND;
                    }
                    struct fat32_dirent *dirent;
                    parse_directory_entry(sector_data + i, dir, start_sector + sector, i, &dirent);
                    // DEBUG_PRINTF("FOUND %s at %d\n, comparing with %s", dirent->name, i, name);
                    if(strcmp(dirent->name, name) == 0) {
                        *retdir = dirent;
                        if(retsector != NULL)
                            *retsector = start_sector + sector;
                        if(retoffset != NULL)
                            *retoffset = i;
                        if(retcluster != NULL)
                            *retcluster = cluster;
                        return SYS_ERR_OK;
                    }
                    free_dirent(dirent, false);
                }
            }
        }
        CHECK_ERR(get_next_cluster(cluster, &cluster), "error getting next cluster");
    }

    return FS_ERR_NOTFOUND;
}

static void create_new_empty_dirent(struct fat32_dirent *parent, char *name, bool is_dir, int Attr, struct fat32_dirent **retent) {
    struct fat32_dirent *ent = malloc(sizeof(struct fat32_dirent));
    ent->size = 0;
    ent->is_dir = is_dir;
    ent->Attr = Attr;
    ent->FstCluster = 0;
    ent->name = malloc(strlen(name) + 1);
    memcpy(ent->name, name, strlen(name));
    ent->name[strlen(name)] = '\0';
    ent->parent = parent;
    *retent = ent;
}

//writes "value" to FAT entry of "cluster"
static errval_t write_to_FAT(int cluster, uint32_t value) {
    errval_t err;

    uint8_t FAT_Sector[SDHC_BLOCK_SIZE];
    int fat_logical_sector = FAT_SECTOR(cluster);
    CHECK_ERR(sd_read_sector(fat_logical_sector, FAT_Sector), "failed to read FAT");
    FAT_Entry *entry = (FAT_Entry *) (FAT_Sector + FAT_OFFSET(cluster));
    *entry = value;

    CHECK_ERR(sd_write_sector(fat_logical_sector, FAT_Sector), "failed to write to FAT");

    return SYS_ERR_OK;
}

//REQUIRES last_cluster to be dir's final cluster, otherwise function will break
//if last_cluster is -1, skip modifying the existing directory entry
static errval_t extend_dirent_by_one_cluster(struct fat32_dirent *dir, int last_cluster, int *retcluster) {
    errval_t err;

    // DEBUG_PRINTF("Extending Fst cluster : %d, last cluster : %d\n", dir->FstCluster, last_cluster);
    CHECK_ERR(allocate_cluster(retcluster), "");

    if(dir->FstCluster == 0)
        dir->FstCluster = *retcluster;

    if(last_cluster > 0) {
        //write newly allocated cluster to FAT of previous last cluster
        CHECK_ERR(write_to_FAT(last_cluster, *retcluster), "failed to write new cluster to FAT of old cluster");
    }
    else if(last_cluster == 0) {
        //write back to the sector of this dirent
        uint8_t dir_data[SDHC_BLOCK_SIZE];
        CHECK_ERR(sd_read_sector(dir->sector, dir_data), "");
        assert(dir->FstCluster == *retcluster);
        marshall_directory_entry(dir, dir_data + dir->sector_offset);
        CHECK_ERR(sd_write_sector(dir->sector, dir_data), "");
    }

    //write EOC to newly allocated cluster
    CHECK_ERR(write_to_FAT(*retcluster, CLUSTER_EOC), "failed to write EOC to FAT");


    return SYS_ERR_OK;
}

static errval_t create_new_directory(struct fat32_dirent *dir) {
    errval_t err;

    int cluster;
    CHECK_ERR(extend_dirent_by_one_cluster(dir, -1, &cluster), "extension failed");
    int sector = FIRST_SECTOR_OF_CLUSTER(cluster);
    uint8_t data[SDHC_BLOCK_SIZE];
    memset(data, 0, SDHC_BLOCK_SIZE);
    //create . dir
    struct fat32_dirent *dot;
    create_new_empty_dirent(dir->parent, ".", true, ATTR_DIRECTORY, &dot);
    dot->FstCluster = dir->FstCluster;
    struct fat32_dirent *dotdot;
    assert(dir->parent != NULL);
    create_new_empty_dirent(dir->parent->parent, "..", true, ATTR_DIRECTORY, &dotdot);
    dotdot->FstCluster = dir->parent->parent == NULL ? 0 : dir->parent->FstCluster; //if root, set 0

    marshall_directory_entry(dot, data);
    marshall_directory_entry(dotdot, data + 32);
    
    
    CHECK_ERR(sd_write_sector(sector, data), "");

    return SYS_ERR_OK;
}

static errval_t create_dirent_in_dir(struct fat32_dirent *curr, char *name, bool is_dir, int Attr, struct fat32_dirent **retent) {
    errval_t err;

    if(!valid_shortname(name)) {
        return FS_ERR_ILLEGAL_NAME;
    }
    struct fat32_dirent *dir;
    create_new_empty_dirent(curr, name, is_dir, Attr, &dir);
    // DEBUG_PRINTF("DONE CREATING EMPTY DIRENT, AT SECTOR AND OFFSET %d, %d\n", dir->sector, dir->sector_offset);

    if(Attr == ATTR_DIRECTORY) {
        CHECK_ERR(create_new_directory(dir), "Failed to create new directory");
    }

    //Find first free dirent space to write newly created directory to
    int sector,offset,cluster;   
    err = find_in_directory(curr, name, true, NULL, &sector, &offset, &cluster);

    //If not enough free space in the current directory, extend
    if(err == FS_ERR_NOTFOUND) {
        int next_cluster; 
        CHECK_ERR(extend_dirent_by_one_cluster(curr, cluster, &next_cluster), "");
        assert(next_cluster >= 2);
        sector = FIRST_SECTOR_OF_CLUSTER(next_cluster);
        offset = 0;
    }
    
    assert(offset % 32 == 0);

    //Update sector and offset of newly created directory
    dir->sector = sector;
    dir->sector_offset = offset;

    //Write newly created directory to appropriate location in current directory 
    uint8_t sector_data[512];
    CHECK_ERR(sd_read_sector(sector, sector_data), "");
    marshall_directory_entry(dir, sector_data + offset);
    CHECK_ERR(sd_write_sector(sector, sector_data), "");

    *retent = dir;

    return SYS_ERR_OK;
}

//given current directory and relative path, find and return dirent
static errval_t search_dirent(struct fat32_dirent *curr, const char *path, bool CREATE_IF_NOT_EXIST, uint8_t Attr, struct fat32_dirent **retent) {
    errval_t err;

    // DEBUG_PRINTF("FINDING %s FROM ROOT %s %d\n", path, curr->name, curr->is_dir);
    bool created = false;
    while(*path != '\0') {

        if(!curr->is_dir) {
            return FS_ERR_NOTDIR;
        }

        int i = 0;
        while(path[i] != '\0' && path[i] != FS_PATH_SEP) i++;
        char *next_dir_name = malloc(i+1);
        memcpy(next_dir_name, path, i);
        next_dir_name[i] = '\0';
        path += i;
        if(*path != '\0')
            path ++;

        struct fat32_dirent *dir;
        // DEBUG_PRINTF("FIND %s in %s\n", next_dir_name, curr->name);
        err = find_in_directory(curr, next_dir_name, false, &dir, NULL, NULL, NULL);
        if(err_is_fail(err)) {
            // DEBUG_PRINTF("FAILED TO FIND! %d,\n", err);
            if((*path == '\0') && CREATE_IF_NOT_EXIST && err == FS_ERR_NOTFOUND) {
                CHECK_ERR(create_dirent_in_dir(curr, next_dir_name, true, Attr , &dir), "failed to create a new directory entry");
                created = true;
            }
            else {
                free(next_dir_name);
                return err_push(err, FS_ERR_NOTFOUND);
            }
        }
        // DEBUG_PRINTF("REMAINING : %s\n");
        free(next_dir_name);
        curr = dir;
    }

    // if(CREATE_IF_NOT_EXIST && !created)
    //     return FS_ERR_EXISTS;
    
    // DEBUG_PRINTF("DONE CREATING/FINDING DIRENT, AT SECTOR AND OFFSET %d, %d\n", curr->sector, curr->sector_offset);
    *retent = curr;

    return SYS_ERR_OK;
}

static void strtoupper(char *s) {
    for(int i=0;i<strlen(s);i++)
        if(isalpha(s[i]))
            s[i] = toupper(s[i]);
}

//given a mount point and a path, find and return the directory entry
static errval_t find_dirent(const char *mount_point, const char *path, bool CREATE_IF_NOT_EXIST, uint8_t Attr, struct fat32_dirent **retent) {
    errval_t err;

    char *clean_path = malloc(strlen(path) + 1);
    strcpy(clean_path, path);
    strtoupper(clean_path);

    bool from_root = strstr(clean_path, mount_point) == clean_path;

    struct fat32_dirent *dir;
    if(from_root) {
        clean_path += strlen(mount_point);
        dir = root_directory;
    }
    else {
        return FS_ERR_NOTFOUND;
    }

    CHECK_ERR_PUSH(search_dirent(dir, clean_path, CREATE_IF_NOT_EXIST, Attr, retent), FS_ERR_SEARCH_FAIL);
    // DEBUG_PRINTF("DONE SEARCHING DIRENT, AT SECTOR AND OFFSET %d, %d\n", dir->sector, dir->sector_offset);

    return SYS_ERR_OK;
}

static errval_t open_dirent(const char *path, struct fat32_handle **rethandle, int ATTR, bool CREATE, errval_t ERR) {
    errval_t err;
    struct fat32_dirent *dir;
    char *clean_path = malloc(strlen(path) + 1);
    strcpy(clean_path, path);
    strtoupper(clean_path);
    CHECK_ERR_PUSH(find_dirent(manager->mount, clean_path, CREATE, ATTR, &dir), FS_ERR_OPEN);
    // DEBUG_PRINTF("DONE FINDING DIRENT AT SECTOR, OFFSET %d, %d\n", dir->sector, dir->sector_offset);
    
    if(!(dir->Attr & ATTR)) {
        return ERR;
    }
    
    struct fat32_handle *handle = malloc(sizeof(struct fat32_handle));
    handle->dirent = dir;
    handle->path = malloc(strlen(clean_path) + 1);
    strncpy(handle->path, clean_path, strlen(clean_path));
    handle->path[strlen(clean_path)] = '\0';
    handle->pos = 0;

    *rethandle = handle;

    return SYS_ERR_OK;
}

//WATCH OUT, THIS FUNCTION WILL SPIT OUT NONSENSE IF CLUSTER DOESNT BELONG TO A directory
//offset is directory number, and NOT directory number * 32
static errval_t is_last_dirent(int cluster, int offset, bool *ret) {
    
    errval_t err;

    //we want the next one
    offset++;

    int sector, offset_into_sector;
    uint8_t data[SDHC_BLOCK_SIZE];
    while(true) {
      err = sector_from_cluster_offset(cluster, offset * 32, &sector, &offset_into_sector);
      if(err_is_fail(err)) {
          if(err == FS_ERR_INDEX_BOUNDS) {
              *ret = true;
              return SYS_ERR_OK;
          }
          else
            return err;
      }
      CHECK_ERR(sd_read_sector(sector, data), "");
      for(; offset_into_sector < SDHC_BLOCK_SIZE; offset_into_sector += 32) {
          if(data[offset_into_sector] == DIR_ALL_FREE) {
              *ret = true;
              return SYS_ERR_OK;
          }
          else if(data[offset_into_sector] != DIR_FREE) {
              *ret = false;
              return SYS_ERR_OK;
          }
          offset++;
      }
    }

    return FS_ERR_IMPOSSIBLE;
}

//frees the entire cluster chain starting at cluster
static errval_t burn_cluster_chain(int cluster) {
    errval_t err;
    while(cluster != CLUSTER_EOC && cluster != CLUSTER_FREE) {
        CHECK_ERR(write_to_FAT(cluster, 0), "");
        push_back(manager->free_clusters, cluster);
        get_next_cluster(cluster, &cluster);
    }
    return SYS_ERR_OK;
}

static errval_t delete_dirent(struct fat32_dirent *dir) {
    errval_t err;

    //cannot delete root directory
    if(dir->parent == NULL)
        return FS_ERR_ROOT_DELETE;

    if(dir->is_dir) {
        bool is_last;
        CHECK_ERR(is_last_dirent(dir->FstCluster, 1, &is_last), "");
        if(!is_last)
            return FS_ERR_NOTEMPTY;
    }

    CHECK_ERR(burn_cluster_chain(dir->FstCluster), "");

    //check if dir is the last directory entry of the directory we are deleting it from
    int parent_sector = FIRST_SECTOR_OF_CLUSTER(dir->parent->FstCluster);
    int offset = (dir->sector + dir->sector_offset - parent_sector)/32;
    bool is_last_in_parent;
    CHECK_ERR(is_last_dirent(dir->parent->FstCluster, offset, &is_last_in_parent), "");

    uint8_t data[SDHC_BLOCK_SIZE];
    CHECK_ERR(sd_read_sector(dir->sector, data), "");
    data[dir->sector_offset] = is_last_in_parent ? DIR_ALL_FREE : DIR_FREE;
    CHECK_ERR(sd_write_sector(dir->sector, data), "");

    return SYS_ERR_OK;
}

void fat32_preinit(void) {
    manager = malloc(sizeof(struct fat32_manager));
}
// Initialize the FAT32 filesystem, get all the necessary information, and populate the free block list with some free blocks
errval_t fat32_init(char *mnt) { 
    errval_t err;

    manager->mount = mnt; 

    uint8_t bpb[SDHC_BLOCK_SIZE];

    CHECK_ERR(sd_read_sector(BPB_SECTOR, bpb), "bad read");

    check_set_bpb_metadata(bpb);

    CHECK_ERR(initialize_free_clusters(), "Failed to find free clusters");

    return SYS_ERR_OK; 
}

errval_t fat32_open(const char *path, fat32_handle_t *rethandle) {
    errval_t err;

    struct fat32_handle *handle;
    CHECK_ERR(open_dirent(path, &handle, ATTR_ARCHIVE, false, FS_ERR_NOTFILE), "failed to open file");
    handle->isdir = false;
    *rethandle = handle;

    return SYS_ERR_OK;
}

errval_t fat32_create(const char *path, fat32_handle_t *rethandle) {
    errval_t err;

    // DEBUG_PRINTF("CREATING %s\n", path);
    struct fat32_handle *handle;
    CHECK_ERR(open_dirent(path, &handle, ATTR_ARCHIVE, true, FS_ERR_NOTFILE), "failed to open file");
    // DEBUG_PRINTF("DONE OPENING DIRENT, AT SECTOR AND OFFSET %d, %d\n", handle->dirent->sector, handle->dirent->sector_offset);
    handle->isdir = false;
    *rethandle = handle;

    return SYS_ERR_OK;
}

errval_t fat32_opendir(const char *path, fat32_handle_t *rethandle) {
    errval_t err;
    
    // DEBUG_PRINTF("FAT32 OPENING DIR %s\n", path);
    struct fat32_handle *handle;
    CHECK_ERR(open_dirent(path, &handle, ATTR_DIRECTORY, false, FS_ERR_NOTDIR), "failed to open dir");
    handle->isdir = true;
    handle->pos = 0;
    *rethandle = handle;

    return SYS_ERR_OK;
}

//assumes retname is malloced large enough
errval_t fat32_dir_read_next(fat32_handle_t inhandle, char **retname, struct fs_fileinfo *info) {
    errval_t err;
    struct fat32_handle *handle = inhandle;
    // DEBUG_PRINTF("READING NEXT OF %s\n AT POS %d", handle->dirent->name, handle->pos);
    if(!handle->isdir)
        return FS_ERR_NOTDIR;    

    int sector,offset;
    uint8_t dir_block[SDHC_BLOCK_SIZE];
    // DEBUG_PRINTF("");
    CHECK_ERR(sector_from_cluster_offset(handle->dirent->FstCluster, handle->pos * 32, &sector, &offset), "");
    // DEBUG_PRINTF("READING SECTOR AND OFFSET %d, %d\n", sector, offset);
    CHECK_ERR(sd_read_sector(sector, dir_block), "bad read");
    if(dir_block[offset] == DIR_ALL_FREE) {
        return FS_ERR_INDEX_BOUNDS;
    }
    else if(dir_block[offset] == DIR_FREE) {
        handle->pos++;
        return fat32_dir_read_next(inhandle, retname, info);
    }

    struct fat32_dirent *dir;
    parse_directory_entry(dir_block + offset, handle->dirent, sector, offset, &dir);
    *retname = dir->name;
    if(info) {
        info->size = dir->size;
        info->type = dir->is_dir ? FS_DIRECTORY : FS_FILE;
    }

    handle->pos++;

    // DEBUG_PRINTF("DIR READ NEXT SUCCESS, READ %s\n", *retname);
    return SYS_ERR_OK;
}

errval_t fat32_tell(fat32_handle_t handle, size_t *pos) {
    struct fat32_handle *h = handle;
    if(h->isdir)
        return FS_ERR_NOTFILE;
    *pos = h->pos;
    return SYS_ERR_OK;
}

errval_t fat32_seek(fat32_handle_t handle, enum fs_seekpos whence, off_t offset) {
    struct fat32_handle *h = handle;
    if(h->isdir)
        return FS_ERR_NOTFILE;

    switch(whence) {
        case FS_SEEK_SET:
            h->pos = MIN(offset, h->dirent->size);
            break;
        case FS_SEEK_CUR:
            h->pos = MIN(h->pos + offset, h->dirent->size);
            break;
        case FS_SEEK_END:
            h->pos = MIN(h->dirent->size - offset, h->dirent->size);
            break;
    }

    return SYS_ERR_OK;
}

static void close_handle(struct fat32_handle *handle) {
    free(handle->path);
    free_dirent(handle->dirent, false);
    free(handle);
}

errval_t fat32_close(fat32_handle_t inhandle) {
    struct fat32_handle *handle = inhandle;
    if(handle->isdir)
        return FS_ERR_NOTFILE;
    close_handle(handle);
    return SYS_ERR_OK;
}

errval_t fat32_closedir(fat32_handle_t inhandle) {
    struct fat32_handle *handle = inhandle;
    if(!handle->isdir)
        return FS_ERR_NOTDIR;
    close_handle(handle);
    return SYS_ERR_OK;
}

errval_t fat32_mkdir(const char *path) {
    errval_t err;

    struct fat32_dirent *h;

    CHECK_ERR(find_dirent(manager->mount, path, true, ATTR_DIRECTORY, &h), "mkdir failed");

    return SYS_ERR_OK;
}

errval_t fat32_read(fat32_handle_t handle, void *buffer, size_t bytes, size_t *bytes_read) {
    errval_t err;

    struct fat32_handle *fhandle = handle;
    //read first block
    uint8_t data[SDHC_BLOCK_SIZE];

    size_t start_bytes = bytes;
    while(bytes != 0 && fhandle->pos != fhandle->dirent->size) {
        //we read every iteration, because we either read a new sector, or we terminate
        int sector, offset;
        CHECK_ERR(sector_from_cluster_offset(fhandle->dirent->FstCluster, fhandle->pos, &sector, &offset), "");
        CHECK_ERR(sd_read_sector(sector, data), "bad read");

        size_t cpy_bytes = MIN(fhandle->dirent->size - fhandle->pos, MIN(SDHC_BLOCK_SIZE - offset, bytes));
        memcpy(buffer, data + offset, cpy_bytes);
        buffer += cpy_bytes;
        fhandle->pos += cpy_bytes;
        bytes -= cpy_bytes;
    }

    if(bytes_read != NULL)
        *bytes_read = start_bytes - bytes;
    
    if(start_bytes == bytes)
        return FS_ERR_EOF;

    return SYS_ERR_OK;
}

errval_t fat32_write(fat32_handle_t handle, const void *buffer, size_t bytes, size_t *bytes_written) {
    errval_t err;
    struct fat32_handle *fhandle = handle;
    //DEBUG_PRINTF("FAT32 WRITING %d bytes into %s, at sector and offset %d, %d\n", bytes, fhandle->dirent->name, fhandle->dirent->sector, fhandle->dirent->sector_offset);
    uint8_t data[SDHC_BLOCK_SIZE];
    size_t start_bytes = bytes;
    while(bytes != 0) {
        //we read every iteration, because we either read a new sector, or we terminate
        int sector, offset;
        
        err = sector_from_cluster_offset(fhandle->dirent->FstCluster, fhandle->pos, &sector, &offset);
        if(err_is_fail(err)) {
            if(err == FS_ERR_INDEX_BOUNDS) {
                //out of space, extend the file, write to bytes in case we throw an error
                if(bytes_written)
                    *bytes_written = start_bytes - bytes;

                int last_cluster;
                CHECK_ERR(get_last_cluster(fhandle->dirent->FstCluster, &last_cluster), "");
                CHECK_ERR(extend_dirent_by_one_cluster(fhandle->dirent, last_cluster, &last_cluster), "");
                sector = FIRST_SECTOR_OF_CLUSTER(last_cluster);
                offset = 0;
            }
            else
                return err;
        }

        size_t cpy_bytes = MIN(SDHC_BLOCK_SIZE - offset, bytes);
        if(cpy_bytes != SDHC_BLOCK_SIZE)
            CHECK_ERR(sd_read_sector(sector, data), "bad read");

        memcpy(data + offset, buffer, cpy_bytes);
        CHECK_ERR(sd_write_sector(sector, data), "bad write");
        buffer += cpy_bytes;
        fhandle->pos += cpy_bytes;
        bytes -= cpy_bytes;
    }

    //write new size back to dirent
    if(start_bytes - bytes > 0) {
        fhandle->dirent->size = fhandle->pos;
        CHECK_ERR(sd_read_sector(fhandle->dirent->sector, data), "");
        marshall_directory_entry(fhandle->dirent, data + fhandle->dirent->sector_offset);
        CHECK_ERR(sd_write_sector(fhandle->dirent->sector, data), "");
    }

    if(bytes_written)
        *bytes_written = start_bytes - bytes;
    
    if(start_bytes == bytes)
        return FS_ERR_EOF;
    
    return SYS_ERR_OK;
}

errval_t fat32_rmdir(const char *path) {
    errval_t err;
    struct fat32_dirent *dir;
    CHECK_ERR(find_dirent(manager->mount, path, false, ATTR_DIRECTORY, &dir), "");

    CHECK_ERR_PUSH(delete_dirent(dir), FS_ERR_DELETE_DIR);

    return SYS_ERR_OK;
}

errval_t fat32_remove(const char *path) {
    errval_t err;
    struct fat32_dirent *dir;
    CHECK_ERR(find_dirent(manager->mount, path, false, ATTR_ARCHIVE, &dir), "");

    CHECK_ERR_PUSH(delete_dirent(dir), FS_ERR_DELETE_DIR);

    return SYS_ERR_OK;
}

errval_t fat32_stat(fat32_handle_t inhandle, struct fs_fileinfo *info) {
    struct fat32_handle *fhandle = inhandle;
    // DEBUG_PRINTF("RECEIVED HANDLE TO %s of %d\n", fhandle->dirent->name, fhandle->isdir);
    info->type = fhandle->isdir ? FS_DIRECTORY : FS_FILE;
    info->size = fhandle->dirent->size;
    return SYS_ERR_OK;
}
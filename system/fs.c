#include <xinu.h>
#include <kernel.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#ifdef FS
#include <fs.h>

static struct fsystem fsd;
int dev0_numblocks;
int dev0_blocksize;
char *dev0_blocks;

extern int dev0;
int storedValueForBlock;

char block_cache[512];

#define SB_BLK 0
#define BM_BLK 1
#define RT_BLK 2

#define NUM_FD 16
struct filetable oft[NUM_FD]; // open file table
int next_open_fd = 0;
int inodenum=0;


#define INODES_PER_BLOCK (fsd.blocksz / sizeof(struct inode))
#define NUM_INODE_BLOCKS (( (fsd.ninodes % INODES_PER_BLOCK) == 0) ? fsd.ninodes / INODES_PER_BLOCK : (fsd.ninodes / INODES_PER_BLOCK) + 1)
#define FIRST_INODE_BLOCK 2

int fs_fileblock_to_diskblock(int dev, int fd, int fileblock);

int fs_fileblock_to_diskblock(int dev, int fd, int fileblock) {
  int diskblock;

  if (fileblock >= INODEBLOCKS - 2) {
    printf("No indirect block support\n");
    return SYSERR;
  }

  diskblock = oft[fd].in.blocks[fileblock]; //get the logical block address

  return diskblock;
}

/* read in an inode and fill in the pointer */
int fs_get_inode_by_num(int dev, int inode_number, struct inode *in) {
  int bl, inn;
  int inode_off;

  if (dev != 0) {
    printf("Unsupported device\n");
    return SYSERR;
  }
  if (inode_number > fsd.ninodes) {
    printf("fs_get_inode_by_num: inode %d out of range\n", inode_number);
    return SYSERR;
  }

  bl = inode_number / INODES_PER_BLOCK;
  inn = inode_number % INODES_PER_BLOCK;
  bl += FIRST_INODE_BLOCK;

  inode_off = inn * sizeof(struct inode);

  /*
  printf("in_no: %d = %d/%d\n", inode_number, bl, inn);
  printf("inn*sizeof(struct inode): %d\n", inode_off);
  */

  bs_bread(dev0, bl, 0, &block_cache[0], fsd.blocksz);
  memcpy(in, &block_cache[inode_off], sizeof(struct inode));

  return OK;

}

/* write inode indicated by pointer to device */
int fs_put_inode_by_num(int dev, int inode_number, struct inode *in) {
  int bl, inn;

  if (dev != 0) {
    printf("Unsupported device\n");
    return SYSERR;
  }
  if (inode_number > fsd.ninodes) {
    printf("fs_put_inode_by_num: inode %d out of range\n", inode_number);
    return SYSERR;
  }

  bl = inode_number / INODES_PER_BLOCK;
  inn = inode_number % INODES_PER_BLOCK;
  bl += FIRST_INODE_BLOCK;

  /*
  printf("in_no: %d = %d/%d\n", inode_number, bl, inn);
  */

  bs_bread(dev0, bl, 0, block_cache, fsd.blocksz);
  memcpy(&block_cache[(inn*sizeof(struct inode))], in, sizeof(struct inode));
  bs_bwrite(dev0, bl, 0, block_cache, fsd.blocksz);

  return OK;
}

/* create file system on device; write file system block and block bitmask to
 * device */
int fs_mkfs(int dev, int num_inodes) {
  int i;

  if (dev == 0) {
    fsd.nblocks = dev0_numblocks;
    fsd.blocksz = dev0_blocksize;
  }
  else {
    printf("Unsupported device\n");
    return SYSERR;
  }

  if (num_inodes < 1) {
    fsd.ninodes = DEFAULT_NUM_INODES;
  }
  else {
    fsd.ninodes = num_inodes;
  }

  i = fsd.nblocks;
  while ( (i % 8) != 0) {i++;}
  fsd.freemaskbytes = i / 8;

  if ((fsd.freemask = getmem(fsd.freemaskbytes)) == (void *)SYSERR) {
    printf("fs_mkfs memget failed.\n");
    return SYSERR;
  }

  /* zero the free mask */
  for(i=0;i<fsd.freemaskbytes;i++) {
    fsd.freemask[i] = '\0';
  }

  fsd.inodes_used = 0;

  /* write the fsystem block to SB_BLK, mark block used */
  fs_setmaskbit(SB_BLK);
  bs_bwrite(dev0, SB_BLK, 0, &fsd, sizeof(struct fsystem));

  /* write the free block bitmask in BM_BLK, mark block used */
  fs_setmaskbit(BM_BLK);
  bs_bwrite(dev0, BM_BLK, 0, fsd.freemask, fsd.freemaskbytes);

  return 1;
}


/* print information related to inodes*/
void fs_print_fsd(void) {

  printf("fsd.ninodes: %d\n", fsd.ninodes);
  printf("sizeof(struct inode): %d\n", sizeof(struct inode));
  printf("INODES_PER_BLOCK: %d\n", INODES_PER_BLOCK);
  printf("NUM_INODE_BLOCKS: %d\n", NUM_INODE_BLOCKS);
}

/* specify the block number to be set in the mask */
int fs_setmaskbit(int b) {
  int mbyte, mbit;
  mbyte = b / 8;
  mbit = b % 8;

  fsd.freemask[mbyte] |= (0x80 >> mbit);
  return OK;
}

/* specify the block number to be read in the mask */
int fs_getmaskbit(int b) {
  int mbyte, mbit;
  mbyte = b / 8;
  mbit = b % 8;

  return( ( (fsd.freemask[mbyte] << mbit) & 0x80 ) >> 7);
  return OK;

}

/* specify the block number to be unset in the mask */
int fs_clearmaskbit(int b) {
  int mbyte, mbit, invb;
  mbyte = b / 8;
  mbit = b % 8;

  invb = ~(0x80 >> mbit);
  invb &= 0xFF;

  fsd.freemask[mbyte] &= invb;
  return OK;
}

/* This is maybe a little overcomplicated since the lowest-numbered
   block is indicated in the high-order bit.  Shift the byte by j
   positions to make the match in bit7 (the 8th bit) and then shift
   that value 7 times to the low-order bit to print.  Yes, it could be
   the other way...  */
void fs_printfreemask(void) { // print block bitmask
  int i,j;

  for (i=0; i < fsd.freemaskbytes; i++) {
    for (j=0; j < 8; j++) {
      printf("%d", ((fsd.freemask[i] << j) & 0x80) >> 7);
    }
    if ( (i % 8) == 7) {
      printf("\n");
    }
  }
  printf("\n");
}

/**** Open Function Impementation***/
int fs_open(char *filename, int flags) {
 

 if(flags != O_RDONLY && flags!= O_WRONLY && flags!= O_RDWR){
   
    return SYSERR;
 }
  int i;
  int inode = -1;
  int get_inode_return_value = 0;
  int length;
  int j=0;
  length = strnlen(filename, FILENAMELEN+1);

  if(length > FILENAMELEN){
  printf("Wrong filename length\n");
    return SYSERR;
  }
  int flag=0;
  for(i=0; i<DIRECTORY_SIZE; i++){
  
        if((strncmp(filename, fsd.root_dir.entry[i].name, FILENAMELEN)==0) &&  (fsd.root_dir.entry[i].inode_num >0)){
      
           inode = fsd.root_dir.entry[i].inode_num;
           flag=1;
            break;

        }else{

            flag=0;
        }
  }

  if ( i>=DIRECTORY_SIZE || flag==0 || inode <= 0){
  printf("There is no such file or directory exists \n");
  return SYSERR;
  }
 
for(j=0; j<FILETABLEN;j++){
  
    if((oft[j].in.id == inode ) && strcmp(filename, oft[j].de->name)==0){
   
    if(oft[j].state == FSTATE_OPEN || oft[j].state == 1){
      return SYSERR;
      }else{
    
        oft[j].state = FSTATE_OPEN;
        oft[j].flags = flags;
        oft[j].fileptr = 0;
        strcpy(oft[j].de->name, filename);
        return j;
      }
    }
}

for(j=0; j<FILETABLEN;j++){
    if(oft[j].state == FSTATE_CLOSED || oft[j].state==0){
   kprintf("\n\n Adding to oft\n");
    oft[j].in.id = inode;
    oft[j].state = FSTATE_OPEN;
    oft[j].flags = flags;
    oft[j].fileptr = 0;
     oft[j].de= (  struct dirent* )getmem(sizeof( struct dirent));
      strcpy(oft[j].de->name, filename);

    return j;
 }

}


 
  return SYSERR;

}

/**** Close Function Impementation***/
int fs_close(int fd) {

  if(fd<16){
  
    oft[fd].state=FSTATE_CLOSED;
   
    return OK;
  }
  return SYSERR;
}

/**** Create Function Impementation***/
int fs_create(char *filename, int mode) {

int j;
int data=17;
    int length;
    int i;
    int inode=SYSERR;
    int fd;
    if(mode != O_CREAT){
        printf("Invalid mode\n");
        return -1;
    }
    length = strnlen(filename, FILENAMELEN);
    if(length > FILENAMELEN){
    printf("File name is too long\n");
    return -1;
    }

    for(i=0; i<DIRECTORY_SIZE; i++){
            if((strncmp(filename, fsd.root_dir.entry[i].name, FILENAMELEN)==0)){
                inode =  fs_get_inode_by_num(0,i,&oft[i].in);
                break;

            }
      }

    if(inode != SYSERR){
        printf("File already exists\n");
        return -1;
    }

   fsd.inodes_used = fsd.inodes_used + 1;
   fsd.root_dir.numentries = fsd.root_dir.numentries + 1;

   for(i=0; i<DIRECTORY_SIZE;i++){
   
           if(fsd.root_dir.entry[i].inode_num == 0){
               break;
               }

   }
   j=i;
   for(i=0;i<FILETABLEN;i++){
   
        if(oft[i].in.id == 0){
            break;
         }
   }
    
   if(i>=FILETABLEN){
    printf("inodes are full\n");
    return -1;
   }

   for(i=0;i<16;i++){
               if(oft[i].state == 0){
               break;
           }
      }
   fd=i;
   if(i>FILETABLEN){
           printf("Open file table is full\n");
           return -1;
      }
   
   struct inode in;
   in.id = j+1;
   in.type = INODE_TYPE_FILE;
   in.nlink = 1;
   in.device = dev0;
   in.size = 0;
   oft[fd].in.id = j+1;
   oft[fd].state = FSTATE_OPEN;
   oft[fd].state = 1;
   oft[fd].in.type = INODE_TYPE_FILE;
   oft[fd].in.nlink = 1;
   oft[fd].in.device =  dev0;
   oft[fd].in.size = 0;
   oft[fd].fileptr = 0;
   oft[fd].de= (  struct dirent* )getmem(sizeof( struct dirent));
   strncpy(oft[fd].de->name, filename, length);
  
    int m=0;
    int data1=17;
    while(m<12){
        if(fs_getmaskbit(data1) == 0){
            in.blocks[m] = data1;
            data1++;
            m+=1;
        }else{
        m=0;
        }
    }

   fsd.root_dir.entry[j].inode_num = j+1;
   strncpy(fsd.root_dir.entry[j].name, filename, length);
   fs_put_inode_by_num(0,j+1 , &in);
   struct inode in1;
   fs_get_inode_by_num(0, j+1, &in1);
   kprintf("\n Creation verification %d %d", in1.id, in1.blocks[0]);
    oft[fd].state = FSTATE_OPEN;
   oft[fd].state = 1;

   
   return fd;
}

/**** Seek Function Impementation***/
int fs_seek(int fd, int offset) {
   
    oft[fd].fileptr = oft[fd].fileptr+offset;
  return oft[fd].fileptr;
}



/**** Read Function Impementation***/
int fs_read(int fd, void *buf, int nbytes) {


  int n_blocks=nbytes/512;
  int blocks_to_read;

  int i=0;
  int totalNBytes = nbytes;
  int length;

  
  int offset=0;
  int returnValue=0;
  int block_read = 0;
  char Data_to_read[MDEV_BLOCK_SIZE*4];
kprintf("\n FD = %d inode = %d", storedValueForBlock, oft[fd].in.id);
    if(oft[fd].flags == O_WRONLY){
      return 0;
    }
    if(oft[fd].in.id == -1 || oft[fd].in.id == 0 ){
        return SYSERR;
      }

    struct inode in;
 if(fs_get_inode_by_num(0, oft[fd].in.id, &in) != SYSERR){
    for(int m=0; m<12;m++){
        kprintf("\n Bloc number is %d", in.blocks[m]);
      }
    if(in.blocks[0] > 100){
        block_read = storedValueForBlock;
    }else{
        block_read = in.blocks[0];
    }
    kprintf("\n %d fd value is and inode value is %d, %d ", fd, oft[fd].in.id);

    kprintf("\n inode block number read is %d %d", in.blocks[0], in.nlink);
    if(bs_bread(dev0, block_read, oft[fd].fileptr, buf, nbytes) == SYSERR){
      return 0;
    }
    oft[fd].fileptr += nbytes;
    return nbytes;

    }
    return SYSERR;



   
}

/**** Write Function Impementation***/
int fs_write(int fd, void *buf, int nbytes) {

  int data=17;
  int blockindex=0;
  int returnSize = nbytes;
  int num_blocks= nbytes/512;
  int offset =0;
  int block_write=0;
  int bytes_to_write=0;
      char dataRead[MDEV_BLOCK_SIZE*4];
  

  if(oft[fd].flags= O_RDONLY){
    return SYSERR;
  }
  int i=0;
  struct inode in;
  fs_get_inode_by_num(0, oft[fd].in.id,  &in);
  storedValueForBlock = in.blocks[0];
  for(int m=0; m<12;m++){
    kprintf("\n Bloc number is %d", in.blocks[m]);
  }

  

  kprintf("\n inode block number is %d inode is %d", in.blocks[0], oft[fd].in.id);
  if(bs_bwrite(dev0, in.blocks[0], 0, buf, nbytes) == SYSERR){
    return 0;
  }


  oft[fd].fileptr += nbytes;
  return nbytes;
 
}

/**** Link Function Impementation***/
int fs_link(char *src_filename, char* dst_filename) {
  int i;
  int temp;
  int j;
  int fd;
  int length;
  int k;
  int flag=0;

  for(int i=0; i<DIRECTORY_SIZE; i++){
  if(strcmp(src_filename, fsd.root_dir.entry[i].name) == 0){

              for(j=0; j<DIRECTORY_SIZE;j++){
                
                         if(fsd.root_dir.entry[j].inode_num == 0 ||fsd.root_dir.entry[j].inode_num == -1){
                         fsd.root_dir.numentries = fsd.root_dir.numentries+1;
                                fsd.root_dir.entry[j].inode_num = fsd.root_dir.entry[i].inode_num;
                                length = strnlen(dst_filename, FILENAMELEN);
                                strncpy(fsd.root_dir.entry[j].name, dst_filename, length);

                                struct inode in;
                               
                                in.nlink+=1;
                              
                                 fs_put_inode_by_num(0,fsd.root_dir.entry[i].inode_num , &in);
                               
                                return OK;
                           

                             }


                 }
               
        }

  }
 
  return SYSERR;

 

}

/**** Unlink Function Impementation***/
int fs_unlink(char *filename) {

int i;
int j;
int k;
int flag=0;

int inode_found = 0;

  for(int i=0; i<DIRECTORY_SIZE; i++){
    if(strcmp(filename, fsd.root_dir.entry[i].name) == 0){
        struct inode in;
        inode_found = fsd.root_dir.entry[i].inode_num;
        fs_get_inode_by_num(0, fsd.root_dir.entry[i].inode_num, &in);
        if(in.nlink > 1){
                         kprintf("\n Entered third if condition\n");
                            fsd.root_dir.entry[i].inode_num = 0;
                            in.nlink = in.nlink-1;
                             memset(fsd.root_dir.entry[i].name, 0 ,FILENAMELEN);
                             fs_put_inode_by_num(0, fsd.root_dir.entry[i].inode_num, &in);
                            return OK;
                         }else if(in.nlink== 1){
                            fsd.root_dir.entry[i].inode_num = 0;
                            memset(fsd.root_dir.entry[i].name, 0 ,FILENAMELEN);
                            for(j=0; j<NUM_INODE_BLOCKS; i++){
                                 fs_clearmaskbit(in.blocks[j]);
                            }
                            fs_put_inode_by_num(0, fsd.root_dir.entry[i].inode_num, &in);
                      }
            kprintf("\n Entered first if condition\n");
          for(k=0; k<FILETABLEN; k++){
              if((oft[k].in.id == inode_found ) && strcmp(oft[k].de->name,filename)==0){
              kprintf("\n Entered second if condition\n");

              oft[k].in.id = 0;
              memset(oft[k].de->name, 0 ,FILENAMELEN);
                    return OK;
                 }
              }
          }
       }


    for(int i=0; i<DIRECTORY_SIZE; i++){
        kprintf("Name of file = %s, inode = %d\n",fsd.root_dir.entry[i].name, fsd.root_dir.entry[i].inode_num );
    }
return SYSERR;

}
#endif /* FS */
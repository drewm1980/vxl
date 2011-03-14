#include "boxm2_block.h"
//:
// \file
#include <boxm2/boxm2_util.h>

boxm2_block::boxm2_block(boxm2_block_id id, char* buff)
{
  block_id_ = id;
  buffer_ = buff;
  this->b_read(buff);
}

boxm2_block::boxm2_block(boxm2_block_metadata data)
{
  block_id_ = data.id_;
  this->init_empty_block(data);
}


bool boxm2_block::b_read(char* buff)
{
    long bytes_read = 0;

    //0. first 8 bytes denote size
    vcl_memcpy(&byte_count_, buff, sizeof(byte_count_));
    bytes_read += sizeof(byte_count_);

    //1. read init level, max level, max mb
    vcl_memcpy(&init_level_, buff+bytes_read, sizeof(init_level_));
    bytes_read += sizeof(init_level_);
    vcl_memcpy(&max_level_, buff+bytes_read, sizeof(max_level_));
    bytes_read += sizeof(max_level_);
    vcl_memcpy(&max_mb_, buff+bytes_read, sizeof(max_mb_));
    bytes_read += sizeof(max_mb_);

    //2. read in sub block dimension, sub block num
    double dims[4];
    vcl_memcpy(&dims, buff+bytes_read, sizeof(dims));
    bytes_read += sizeof(dims);
    int    nums[4];
    vcl_memcpy(&nums, buff+bytes_read, sizeof(nums));
    bytes_read += sizeof(nums);
    sub_block_dim_ = vgl_vector_3d<double>(dims[0], dims[1], dims[2]);
    sub_block_num_ = vgl_vector_3d<unsigned>(nums[0], nums[1], nums[2]);

    //3.  read number of buffers
    int numBuffers;
    vcl_memcpy(&numBuffers, buff+bytes_read, sizeof(numBuffers));
    bytes_read += sizeof(numBuffers);

    //3.a read length of tree buffers
    int treeLen;
    vcl_memcpy(&treeLen, buff+bytes_read, sizeof(treeLen));
    bytes_read += sizeof(treeLen);

    //4. setup big arrays (3d block of trees)
    uchar16* treesBuff = (uchar16*) (buff+bytes_read);
    trees_     = boxm2_array_3d<uchar16>( sub_block_num_.x(),
                                          sub_block_num_.y(),
                                          sub_block_num_.z(),
                                          treesBuff);
    bytes_read += sizeof(uchar16)*sub_block_num_.x()*sub_block_num_.y()*sub_block_num_.z();

    //5. 2d array of tree pointers
    int* treePtrsBuff = (int*) (buff+bytes_read);
    tree_ptrs_ = boxm2_array_2d<int>( numBuffers, treeLen, treePtrsBuff);
    bytes_read += sizeof(int) * numBuffers * treeLen;

    //6. 1d aray of trees_in_buffers
    ushort* treesCountBuff = (ushort*) (buff+bytes_read);
    trees_in_buffers_ = boxm2_array_1d<ushort>(numBuffers, treesCountBuff);
    bytes_read += sizeof(ushort) * numBuffers;

    //7. 1d array of mem pointers
    ushort2* memPtrsBuff = (ushort2*) (buff+bytes_read);
    mem_ptrs_ = boxm2_array_1d<ushort2>(numBuffers, memPtrsBuff);
    bytes_read += sizeof(ushort2) * numBuffers;
    vcl_cout << *this;
    return true;
}

//:
//  This type of writing is sort of counter intuitive, as the buffer
//  just needs to be returned and written to disk. The first few calls
//  ensure the meta data is lined up correctly.  To use this, just pass in
//  the boxm2_block buffer.
bool boxm2_block::b_write(char* buff)
{
    long bytes_written = 0;

    //0. writing total size
    vcl_memcpy(buff, &byte_count_, sizeof(byte_count_));
    bytes_written += sizeof(byte_count_);

    //1. write init level, max level, max mb
    vcl_memcpy(buff+bytes_written, &init_level_, sizeof(init_level_));
    bytes_written += sizeof(init_level_);
    vcl_memcpy(buff+bytes_written, &max_level_, sizeof(max_level_));
    bytes_written += sizeof(max_level_);
    vcl_memcpy(buff+bytes_written, &max_mb_, sizeof(max_mb_));
    bytes_written += sizeof(max_mb_);

    //2. Write sub block dimension, sub block num
    double dims[4] = {sub_block_dim_.x(), sub_block_dim_.y(), sub_block_dim_.z(), 0.0};
    vcl_memcpy(buff+bytes_written, dims, 4 * sizeof(double));
    bytes_written += 4 * sizeof(double);

    int nums[4] = {sub_block_num_.x(), sub_block_num_.y(), sub_block_num_.z(), 0 };
    vcl_memcpy(buff+bytes_written, nums, 4 * sizeof(int));
    bytes_written += 4 * sizeof(int);

    //3.  write number of buffers
    int numBuffers = tree_ptrs_.rows();
    vcl_memcpy(buff+bytes_written, &numBuffers, sizeof(numBuffers));
    bytes_written += sizeof(numBuffers);

    //3.a write length of tree buffers
    int treeLen = tree_ptrs_.cols();
    vcl_memcpy(buff+bytes_written, &treeLen, sizeof(treeLen));
    bytes_written += sizeof(treeLen);

    //the arrays themselves should be already in the char buffer, so no need to copy
    return true;
}


//: initializes empty scene given
// This method uses the max_mb parameter to determine how many data cells to
// allocate.  MAX_MB is assumed to include blocks, alpha, mog3, num_obs and 16 byte aux data
bool boxm2_block::init_empty_block(boxm2_block_metadata data)
{
  //calc max number of bytes, data buffer length, and alpha init (consts)
  const int MAX_BYTES    = int(data.max_mb_)*1024*1024;
  const int BUFF_LENGTH  = 1L<<16; // 65536

  //total number of (sub) blocks in the scene
  int total_blocks =  data.sub_block_num_.x()
                    * data.sub_block_num_.y()
                    * data.sub_block_num_.z();

  //to initialize
  int num_buffers, blocks_per_buffer;
  if (data.random_)
  {
    int blockBytes = total_blocks*(sizeof(int) + 16*sizeof(char)); //16 byte tree, 4 byte int pointer
    int freeBytes = MAX_BYTES - blockBytes;
    int dataSize  = 8*sizeof(char) +    //MOG
                    4*sizeof(short) +   //numObs
                    sizeof(float) +     //alpha
                    4*sizeof(int);      //aux data (cum_seg_len
    int num_cells = (int) (freeBytes/dataSize);                         //number of cells given maxmb
    num_buffers = (int) vcl_ceil( ((float)num_cells/(float)BUFF_LENGTH) );
    blocks_per_buffer = (int) vcl_ceil((float)total_blocks/(float)num_buffers);
    if (num_buffers * BUFF_LENGTH <= total_blocks && !data.random_) {
      vcl_cerr<<"**************************************************\n"
              <<"*** boxm2_block::init_empty_block: ERROR!!!!\n"
              <<"*** Max scene size not large enough to accommodate scene dimensions\n"
              <<"*** cells allocated:  "<<num_buffers * BUFF_LENGTH<<'\n'
              <<"*** total subblocks:  "<<total_blocks<<'\n'
              <<"**************************************************\n";
      return false;
    }

    vcl_cout<<"OCL Scene buffer shape: ["
            <<num_buffers<<" buffers by "
            <<BUFF_LENGTH<<" cells ("
            <<blocks_per_buffer<<" trees per buffer)]. "
            <<"[total tree:"<<num_buffers*BUFF_LENGTH<<']'<<vcl_endl;
  }
  else
  {
    num_buffers = 1;
    blocks_per_buffer = total_blocks;
    vcl_cout<<"Num buffers: "<<num_buffers
            <<" .. num_trees: "<<blocks_per_buffer<<vcl_endl;
  }

  //now construct a byte stream, and read in with b_read
  byte_count_ = calc_byte_count(num_buffers, blocks_per_buffer, total_blocks);
  init_level_ = data.init_level_;
  max_level_  = data.max_level_;
  max_mb_     = int(data.max_mb_);
  buffer_ = new char[byte_count_];

  //get member variable metadata straight, then write to the buffer
  long bytes_read = 0;
  bytes_read += sizeof(byte_count_);   //0. first 8 bytes denote size
  bytes_read += sizeof(init_level_);   //1. read init level, max level, max mb
  bytes_read += sizeof(max_level_);
  bytes_read += sizeof(max_mb_);
  double dims[4]; int nums[4];
  bytes_read += sizeof(dims);          //2. read in sub block dimension, sub block num
  bytes_read += sizeof(nums);
  sub_block_dim_ = data.sub_block_dim_;
  sub_block_num_ = data.sub_block_num_;
  bytes_read += sizeof(num_buffers);        //3.  read number of buffers
  bytes_read += sizeof(blocks_per_buffer);  //3.a read length of tree buffers

  //4. setup big arrays (3d block of trees)
  uchar16* treesBuff = (uchar16*) (buffer_+bytes_read);
  trees_     = boxm2_array_3d<uchar16>( sub_block_num_.x(),
                                        sub_block_num_.y(),
                                        sub_block_num_.z(),
                                        treesBuff);
  bytes_read += sizeof(uchar16)*sub_block_num_.x()*sub_block_num_.y()*sub_block_num_.z();

  //5. 2d array of tree pointers
  int* treePtrsBuff = (int*) (buffer_+bytes_read);
  tree_ptrs_ = boxm2_array_2d<int>( num_buffers, blocks_per_buffer, treePtrsBuff);
  bytes_read += sizeof(int) * num_buffers * blocks_per_buffer;

  //6. 1d aray of trees_in_buffers
  ushort* treesCountBuff = (ushort*) (buffer_+bytes_read);
  trees_in_buffers_ = boxm2_array_1d<ushort>(num_buffers, treesCountBuff);
  for (unsigned int i=0; i<trees_in_buffers_.size(); ++i)
    trees_in_buffers_[i] = 0;
  bytes_read += sizeof(ushort) * num_buffers;

  //7. 1d array of mem pointers
  ushort2* memPtrsBuff = (ushort2*) (buffer_+bytes_read);
  mem_ptrs_ = boxm2_array_1d<ushort2>(num_buffers, memPtrsBuff);
  for (unsigned int i=0; i<mem_ptrs_.size(); ++i) {
    mem_ptrs_[i][0] = 0;
    mem_ptrs_[i][1] = 0;
  }
  bytes_read += sizeof(ushort2) * num_buffers;


  //--- Now initialize blocks and their pointers --------- ---------------------
  if (data.random_)
  {
    //6.a create a random 'iterator'
    int* randIndex = new int[trees_.size()];
    for (unsigned int i=0; i<trees_.size(); ++i) randIndex[i] = i;
    boxm2_util::random_permutation(randIndex, trees_.size());
    int tree_index = 0;
    boxm2_array_3d<uchar16>::iterator iter;
    for (iter = trees_.begin(); iter != trees_.end(); ++iter, ++tree_index)
    {
      //get a random index, convert it to Buffer Index/Offset pair
      int r_index  = randIndex[tree_index];
      int b_index  = r_index / blocks_per_buffer;
      int b_offset = r_index % blocks_per_buffer;

      //store buffer offset in bits [10,11]
      uchar16 treeBlk( (unsigned char) 0 );
      unsigned char off_hi = (unsigned char) (b_offset >> 8);
      unsigned char off_lo = (unsigned char) (b_offset & 255);
      treeBlk[10] = off_hi; treeBlk[11] = off_lo;

      //store buffer index in bits [12,13]
      unsigned char b_hi = (unsigned char) (b_index >> 8);
      unsigned char b_lo = (unsigned char) (b_index & 255);
      treeBlk[12] = b_hi; treeBlk[13] = b_lo;
      for (int i=0; i<16; i++)
        (*iter)[i] = treeBlk[i];

      //make sure mem_ptrs_ and blocks in buffers add up
      mem_ptrs_[b_index][1]++;
      trees_in_buffers_[b_index]++;

      //make sure block pointers points back to the right index
      tree_ptrs_[b_index][b_offset] = tree_index;
    }
    delete [] randIndex;
  }
  else
  {
    //6.a create a random 'iterator'
    int tree_index = 0;
    boxm2_array_3d<uchar16>::iterator iter;
    for (iter = trees_.begin(); iter != trees_.end(); ++iter, ++tree_index)
    {
      //data index is tree index at this point
      //store data index in bits [10, 11, 12, 13] ;
      uchar16 treeBlk( (unsigned char) 0 );
      treeBlk[10] = (tree_index) & 0xff;
      treeBlk[11] = (tree_index>>8)  & 0xff;
      treeBlk[12] = (tree_index>>16) & 0xff;
      treeBlk[13] = (tree_index>>24) & 0xff;
      for (int i=0; i<16; i++)
        (*iter)[i] = treeBlk[i];
    }
  }
  return true;
}


//: Given number of buffers, number of trees in each buffer, and number of total trees (x*y*z number)
// \return size of byte stream
long boxm2_block::calc_byte_count(int num_buffers, int trees_per_buffer, int num_trees)
{
  long toReturn = num_trees * sizeof(uchar16) +                     //3d block pointers
                  num_buffers*trees_per_buffer * sizeof(int) +     //tree pointers
                  num_buffers*(sizeof(ushort) + sizeof(ushort2)) +  //blocks in buffers and mem ptrs
                  sizeof(long) +                      //this number
                  3*sizeof(int) +                     //init level, max level, max_mb
                  4*sizeof(double) +                  //dims
                  6*sizeof(int);                      //nums + numBuffers, treeLen
  return toReturn;
}


//------------ I/O -------------------------------------------------------------
vcl_ostream& operator <<(vcl_ostream &s, boxm2_block& block)
{
  s << "Block ID=" << block.block_id() << vcl_endl;
  s << "Byte Count=" << block.byte_count() << vcl_endl;
  s << "Init level=" << block.init_level() << vcl_endl;
  s << "Max level=" << block.max_level() << vcl_endl;
  s << "Max MB=" << block.max_mb() << vcl_endl;
  s << "Sub Block Dim=" << block.sub_block_dim() << vcl_endl;
  s << "Sub Block Num=" << block.sub_block_num() << vcl_endl;
  
  return s;
}

#if 0
void vsl_b_write(vsl_b_ostream& os, boxm2_block const& scene) {}
void vsl_b_write(vsl_b_ostream& os, const boxm2_block* &p) {}
void vsl_b_write(vsl_b_ostream& os, boxm2_block_sptr& sptr) {}
#endif

//: Binary write boxm2_block to stream.
// DUMMY IMPLEMENTATION: does nothing!
void vsl_b_write(vsl_b_ostream&, boxm2_block_sptr const&) {}

#if 0
void vsl_b_read(vsl_b_istream& is, boxm2_block &scene) {}
void vsl_b_read(vsl_b_istream& is, boxm2_block* p) {}
#endif
//: Binary load boxm2_block from stream.
// DUMMY IMPLEMENTATION: does nothing!
void vsl_b_read(vsl_b_istream&, boxm2_block_sptr&) {}
//: Binary load boxm2_block from stream.
// DUMMY IMPLEMENTATION: does nothing!
void vsl_b_read(vsl_b_istream&, boxm2_block_sptr const&) {}


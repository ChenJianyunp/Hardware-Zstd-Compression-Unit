 // This is a generated file. Use and modify at your own risk.
////////////////////////////////////////////////////////////////////////////////

/*******************************************************************************
Vendor: Xilinx
Associated Filename: main.c
#Purpose: This example shows a basic vector add +1 (constant) by manipulating
#         memory inplace.
*******************************************************************************/

#include <fcntl.h>
#include <stdio.h>
#include <iostream>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <assert.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <CL/opencl.h>
#include <CL/cl_ext.h>
#include <time.h>
#include "xclhal2.h"

#include "mem.h"
#include "bitstream.h"
////////////////////////////////////////////////////////////////////////////////

#define NUM_WORKGROUPS (1)
#define WORKGROUP_SIZE (256)
#define MAX_LENGTH (100*1024*1024) //100 Mb
#define MAX_METADATA 100000
#define MEM_ALIGNMENT 4096
#define MAX_COMPRESSED_SIZE (100*1024*1024) //100 Mb
#define LONGNBSEQ 0x7F00


#if defined(VITIS_PLATFORM) && !defined(TARGET_DEVICE)
#define STR_VALUE(arg)      #arg
#define GET_STRING(name) STR_VALUE(name)
#define TARGET_DEVICE GET_STRING(VITIS_PLATFORM)
#endif

struct stream_ptrs_t{
	BYTE* sequence_streaming_ptr;
	BYTE* literal_streaming_ptr;
};

struct metadata{
	long fse_bits_num;
	long huffman_bits_num;
	unsigned ll_state;
	unsigned ml_state;
	unsigned of_state;
	unsigned sequence_num;
	unsigned literal_num;
};
////////////////////////////////////////////////////////////////////////////////

cl_uint load_file_to_memory(const char *filename, char **result)
{

    cl_uint size = 0;
    FILE *f = fopen(filename, "rb");
    if (f == NULL) {
        *result = NULL;
        return -1; // -1 means file opening fail
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    *result = (char *)malloc(size+1);
    if (size != fread(*result, sizeof(char), size, f)) {
        free(*result);
        return -2; // -2 means file reading fail
    }
    fclose(f);
    (*result)[size] = 0;
    return size;
}

int write_file_from_memory(const char *filename, char* src, size_t size_file){
	FILE * fp;
	int n;

	fp = fopen(filename,"w");
	n=fwrite(src, 1, size_file, fp);
	fclose(fp);
	return n;
}

#define ZSTD_MAGICNUMBER            0xFD2FB528    /* valid since v0.8.0 */
int write_header_frame(void* des){
	unsigned char* op = (unsigned char*)des;

	//write the magic number
	MEM_writeLE32(op, ZSTD_MAGICNUMBER);
	op += 4;

	/******Header only contains two bytes: 1Byte frame_header_descripter and 1Byte windows_descripter***********/
	*op = 0x00; //add frame_header_descripter, |bit7-6: b00, did not provide frame size | bit5: b0,  not single continuous memory | bit2: b0, not checksum | bit1-0: b00  , no dictionary
	op++;
	*op= 0x88; //add Window_Descriptor, windows size is 2^27
	op++;

	return ( op-(unsigned char*)des);
}

int write_sequence_section(void* des, stream_ptrs_t* stream_ptrs, long fse_bits_num, long fse_num_sequence, unsigned ll_state, unsigned ml_state, unsigned of_state, unsigned last_block, unsigned repeat_mode){
	unsigned char* pos = (unsigned char*)des;
	unsigned char ll_type = 2, off_type = 2, ml_type = 2; //FSE_Compressed_Mode
	unsigned int fse_bytes_num;
	size_t fse_slice_num = fse_bits_num/128 + (fse_bits_num%128 != 0);
	unsigned int bit_mask;

	BIT_CStream_t stream;

	BYTE ll_table[] = {0x84, 0x89, 0xae, 0x47, 0xd6, 0x71, 0x24, 0xcb, 0x60, 0xce, 0x28, 0x84, 0xcc, 0xc, 0x8, 0x8, 0x0, 0x0, 0x0, 0x0, 0x0};
	BYTE ml_table[] = {0x73, 0xa3, 0x1, 0x0, 0x6, 0x4, 0xc9, 0xa7, 0xc1, 0x34, 0x8c, 0xf3, 0x30, 0xac, 0x1d, 0x0, 0x0, 0x0};
	BYTE of_table[] = {0x54, 0x80, 0x0, 0x0, 0xc, 0x21, 0x6d, 0x34, 0xd, 0x89, 0xa6, 0xa0, 0x58, 0x44, 0x0, 0x2, 0x92, 0x72, 0x14, 0x6, 0x63, 0x0, 0xf, 0x20, 0x0, 0x0, 0x0, 0xa, 0x0, 0xc0, 0x1, 0x0, 0xc0, 0x10, 0x20, 0x0, 0x0};
	const size_t ll_table_size = 21;
	const size_t ml_table_size = 18;
	const size_t of_table_size = 37;
	const size_t ll_state_log = 9;
	const size_t of_state_log = 8;
	const size_t ml_state_log = 9;

	//the last sequence of the last block is a literal only sequence, this sequence is not stored, so minus one
	if(last_block > 0){
		fse_num_sequence--;
	}

	if(repeat_mode){
		ll_type = 3;
		off_type = 3;
		ml_type = 3;
	}

	//add header of the sequence section
	if (fse_num_sequence < 0x7F)
	    *pos++ = (BYTE)fse_num_sequence;
	else if (fse_num_sequence < LONGNBSEQ)
		pos[0] = (BYTE)((fse_num_sequence>>8) + 0x80), pos[1] = (BYTE)fse_num_sequence, pos+=2;
	else
		pos[0]=0xFF, MEM_writeLE16(pos+1, (U16)(fse_num_sequence - LONGNBSEQ)), pos+=3;

	*pos = (BYTE)((ll_type<<6) + (off_type<<4) + (ml_type<<2));
	pos ++;

	if(repeat_mode == 0 ){ //when repeated mode, no table
		memcpy(pos, ll_table, ll_table_size);
		pos += ll_table_size;
		memcpy(pos, ml_table, ml_table_size);
		pos += ml_table_size;
		memcpy(pos, of_table, of_table_size);
		pos += of_table_size;
	}

	fse_bytes_num = fse_bits_num/8;
	memcpy(pos, stream_ptrs->sequence_streaming_ptr, fse_bytes_num);
	pos += fse_bytes_num;

	stream.bitContainer = *(stream_ptrs->sequence_streaming_ptr+fse_bytes_num);
	stream.bitPos = fse_bits_num%8;
	bit_mask = 0;
	bit_mask = ~ bit_mask;
	bit_mask = ~(bit_mask << stream.bitPos);
	stream.bitContainer = stream.bitContainer & bit_mask;
	stream.startPtr = (char*)des;
	stream.ptr = (char*)pos;
	stream.endPtr = stream.ptr+100; //just avoid overflow

	//add ml state bits
	BIT_addBits(&stream, ml_state, ml_state_log);
	BIT_flushBits(&stream);


	//add off state bits
	BIT_addBits(&stream, of_state, of_state_log);
	BIT_flushBits(&stream);

	//add ll state bits
	BIT_addBits(&stream, ll_state, ll_state_log);
	BIT_flushBits(&stream);

	stream_ptrs->sequence_streaming_ptr += (fse_slice_num*16);

	return BIT_closeCStream(&stream);

}

int write_literal_section_no_compression(void* des, stream_ptrs_t* stream_ptrs, long huffman_original_bytes_num){
	unsigned char* pos = (BYTE*)des;
    U32 flSize = 1 + (huffman_original_bytes_num>31) + (huffman_original_bytes_num>4095);
    size_t huffman_compressed_slice_num = huffman_original_bytes_num/16 + (huffman_original_bytes_num%16 != 0);
    unsigned int hType = 0; //raw block
    switch(flSize)
       {
           case 1: /* 2 - 1 - 5 */
        	   pos[0] = (BYTE)((U32)hType + (huffman_original_bytes_num<<3));
               break;
           case 2: /* 2 - 2 - 12 */
               MEM_writeLE16(pos, (U16)((U32)hType + (1<<2) + (huffman_original_bytes_num<<4)));
               break;
           case 3: /* 2 - 2 - 20 */
               MEM_writeLE32(pos, (U32)((U32)hType + (3<<2) + (huffman_original_bytes_num<<4)));
               break;
           default:   /* not necessary : flSize is {1,2,3} */
               assert(0);
       }
    pos += flSize;
    memcpy(pos, stream_ptrs->literal_streaming_ptr, huffman_original_bytes_num);
    (stream_ptrs->literal_streaming_ptr) += huffman_compressed_slice_num*16;
    return (flSize + huffman_original_bytes_num);
}

int write_literal_section(void* des, stream_ptrs_t* stream_ptrs, long huffman_original_bytes_num, int huffman_compressed_bits_num, U16 stream_size[3]){
	unsigned char* pos = (BYTE*)des;
	size_t huffman_compressed_bytes_num = huffman_compressed_bits_num/8 + (huffman_compressed_bits_num%8 != 0);
	size_t huffman_compressed_slice_num = huffman_compressed_bits_num/128 + (huffman_compressed_bits_num%128 != 0);
	size_t lhsize = 3 + (huffman_compressed_bytes_num > 1024) + (huffman_compressed_bytes_num > 16*1024);

	BYTE huffman_table[] = {0x36, 0x10, 0x0, 0x32, 0x7, 0x0, 0x47, 0x16, 0x5a, 0x8e, 0xc4, 0x9b, 0x64, 0x2, 0xa4, 0xc2, 0x21, 0xf1, 0x2, 0xec, 0x4a, 0x21, 0xe6, 0x32, 0x99, 0x92, 0xe7, 0x34, 0x49, 0x19, 0x26, 0x40, 0x7e, 0x5f, 0x39, 0x81, 0x7c, 0x4f, 0xac, 0x62, 0xe8, 0x2e, 0x8, 0x85, 0x9d, 0x15, 0x26, 0x6e, 0x23, 0xb2, 0x68, 0x94, 0xbc, 0xdf, 0x1};
	int huffman_table_size = 54;

	U32 singleStream = huffman_compressed_bytes_num < 256;
	unsigned int hType = 2; //compressed block
	if(lhsize > 3){
		singleStream = 0;
		return -1;
	}else{
		singleStream = 1;
	}

    /* Build header */
    switch(lhsize)
    {
    case 3: /* 2 - 2 - 10 - 10 */
        {   U32 const lhc = hType + ((!singleStream) << 2) + ((U32)huffman_compressed_bytes_num<<4) + ((U32)huffman_original_bytes_num<<14);
            MEM_writeLE24(pos, lhc);
            pos += 3;
            break;
        }
    case 4: /* 2 - 2 - 14 - 14 */
        {   U32 const lhc = hType + (2 << 2) + ((U32)huffman_compressed_bytes_num<<4) + ((U32)huffman_original_bytes_num<<18);
            MEM_writeLE32(pos, lhc);
            pos += 4;
            break;
        }
    case 5: /* 2 - 2 - 18 - 18 */
        {   U32 const lhc = hType + (3 << 2) + ((U32)huffman_compressed_bytes_num<<4) + ((U32)huffman_original_bytes_num<<22);
            MEM_writeLE32(pos, lhc);
            pos[4] = (BYTE)(huffman_original_bytes_num >> 10);
            pos += 5;
            break;
        }
    default:  /* not possible : lhSize is {3,4,5} */
        assert(0);
    }

    //add huffman tree description
    memcpy(pos, huffman_table, huffman_table_size);
    pos += huffman_table_size;

    //add jump table
    if(singleStream == 0){
    	MEM_writeLE16(pos+0, stream_size[0]);
    	MEM_writeLE16(pos+2, stream_size[1]);
    	MEM_writeLE16(pos+4, stream_size[2]);
    	pos += 6;
    }

    if(singleStream == 1){
    	memcpy(pos, stream_ptrs->literal_streaming_ptr, huffman_compressed_bytes_num);
    	pos += huffman_compressed_bytes_num;
    	stream_ptrs->literal_streaming_ptr += huffman_compressed_slice_num*16;

    }else{
    	int streaming_num_slice = stream_size[0]/8+(stream_size[0]%8 != 0);
    	memcpy(pos, stream_ptrs->literal_streaming_ptr, stream_size[0]);
    	pos += streaming_num_slice*8;

    	streaming_num_slice = stream_size[1]/8+(stream_size[1]%8 != 0);
    	memcpy(pos, stream_ptrs->literal_streaming_ptr, stream_size[1]);
    	pos += streaming_num_slice*8;

    	streaming_num_slice = stream_size[2]/8+(stream_size[2]%8 != 0);
    	memcpy(pos, stream_ptrs->literal_streaming_ptr, stream_size[2]);
    	pos += streaming_num_slice*8;
    }

    printf("huffman_compressed_bits_num: %d \n", huffman_compressed_bits_num);

    return (pos-(BYTE*)des);

}
/**
 * sequence_num: number of sequences
 */
int write_block(void* des, struct metadata current_metadata, stream_ptrs_t* stream_ptrs, int last_block, int no_huffman_compression, unsigned repeat_mode){
	U16 stream_size[3];
	size_t huffman_section_size = 0;
	size_t sequence_section_size =0;
	U32 cBlockHeader24;
	unsigned int block_type = 2; //compressed block
	BYTE* pos = (BYTE*)des + 3;//the header consists of 3 bytes

	//write literal section
	if(no_huffman_compression ==1){
		huffman_section_size = write_literal_section_no_compression((void*)pos, stream_ptrs, current_metadata.literal_num);
	}else{
		huffman_section_size = write_literal_section((void*)pos, stream_ptrs, current_metadata.literal_num, current_metadata.huffman_bits_num, stream_size);
	}
	pos	+= huffman_section_size;

	//write sequence section
	sequence_section_size = write_sequence_section((void*)pos, stream_ptrs, current_metadata.fse_bits_num, current_metadata.sequence_num, current_metadata.ll_state, current_metadata.ml_state, current_metadata.of_state, last_block, repeat_mode);
	pos	+= sequence_section_size;

	//write header
	 cBlockHeader24 = last_block + (((U32)block_type)<<1) + (U32)((sequence_section_size+huffman_section_size) << 3);
	 MEM_writeLE24(des, cBlockHeader24);
	 printf("Length of sequence section: %d, length of literal section: %d \n", sequence_section_size, huffman_section_size);

	return (pos-(BYTE*)des);
}

int format_zstd_stream(struct metadata* metadatas, BYTE* des,int block_num, int no_huffman_compression, void* fse_streaming_ptr, void* huffman_streaming_ptr){

	long fse_stream_num_bytes, huffman_stream_num_bytes;
	int size= 0;
	stream_ptrs_t stream_ptrs;
	BYTE* pos = des;
	int last = 0;
    stream_ptrs.literal_streaming_ptr = (BYTE*)huffman_streaming_ptr;
    stream_ptrs.sequence_streaming_ptr = (BYTE*)fse_streaming_ptr;

	fse_stream_num_bytes = 0;
	huffman_stream_num_bytes = 0;
	//write frame header
    size = write_header_frame(des);
    pos += size;

    //write every block
	for(int i =0; i < block_num;i++){
		long current_fse_num_bytes		= metadatas[i].huffman_bits_num/8 + (metadatas[i].huffman_bits_num%8 != 0);
		fse_stream_num_bytes 			+= current_fse_num_bytes;
		long current_huffman_num_bytes	= metadatas[i].fse_bits_num/8 + (metadatas[i].fse_bits_num%8 != 0);
		huffman_stream_num_bytes		+= current_huffman_num_bytes;

		unsigned repeated_mode;

		if(i == 0){
			repeated_mode =0;
		}else{
			repeated_mode =1;
		}

		if(i == (block_num-1)){
			last = 1;
		}else{
			last = 0;
		}

		size = write_block((void*)pos, metadatas[i], &stream_ptrs, last, no_huffman_compression, repeated_mode);
		pos += size;
		printf("The %d th block starts from %d \n", i,(pos- des));
	}


    return (pos- des);
}

/**
 * Parse the streaming of metadata into structs
 */
void parse_metadata(metadata* metadatas, cl_uint* metadata_streaming, unsigned int block_num, int no_huffman_compression, long* fse_stream_num_bytes, long* huffman_stream_num_bytes){
    *fse_stream_num_bytes = 0;
    *huffman_stream_num_bytes = 0;
    for(unsigned int i =0; i < block_num;i++){
    	long * current_metadata_address = (long*)metadata_streaming+i*8;
    	unsigned * current_metadata_address_uns = (unsigned*)current_metadata_address;
//    	metadatas[i].huffman_bits_num 	= *(current_metadata_address);
    	metadatas[i].literal_num		= *(current_metadata_address_uns);
    	metadatas[i].sequence_num		= *(current_metadata_address_uns+1);
    	metadatas[i].fse_bits_num		= *(current_metadata_address+1);
    	metadatas[i].ll_state			= *(current_metadata_address+2);
    	metadatas[i].ml_state			= *(current_metadata_address+3);
    	metadatas[i].of_state			= *(current_metadata_address+4);

    	long current_fse_num_bytes		= metadatas[i].fse_bits_num/8 + (metadatas[i].fse_bits_num%8 != 0);
    	long current_fse_num_slice		= current_fse_num_bytes/16 + (current_fse_num_bytes%16 != 0); //each slice of fse is 16Bytes
    	*fse_stream_num_bytes 			+= current_fse_num_slice*16;

    	long current_huffman_num_bytes	= metadatas[i].huffman_bits_num/8 + (metadatas[i].huffman_bits_num%8 != 0);
    	if(no_huffman_compression == 1){
    		current_huffman_num_bytes	= metadatas[i].literal_num;
    	}else{
    		current_huffman_num_bytes	= metadatas[i].huffman_bits_num/8 + (metadatas[i].huffman_bits_num%8 != 0);
    	}
    	long current_huffman_num_slices = current_huffman_num_bytes/16 + (current_huffman_num_bytes%16 != 0);
    	*huffman_stream_num_bytes		+= current_huffman_num_slices*16;

    	printf("literal_num: %d, sequence_num: %d, huffman_bits_num: %ld, fse_bits_num: %ld, ll_state: %x, ml_state: %x, of_state: %x \n", metadatas[i].literal_num, metadatas[i].sequence_num, metadatas[i].huffman_bits_num, metadatas[i].fse_bits_num, metadatas[i].ll_state, metadatas[i].ml_state, metadatas[i].of_state);
    }
    printf("Total length of fse bitstream: %ld \n", *fse_stream_num_bytes);
    printf("Total length of huffman bitstream: %ld \n", *huffman_stream_num_bytes);

}


int main(int argc, char** argv)
{

    cl_int err;                            // error code returned from api calls
    cl_uint check_status = 0;
    const cl_uint src_number_of_bytes = 12*1024*1024;
	const cl_uint estimated_number_of_block = 64;
	struct metadata metadatas[MAX_METADATA];
	unsigned int block_num;
	long fse_stream_num_bytes, huffman_stream_num_bytes;
	BYTE* compressed_file;

/***********variables for benchmark****************/
	clock_t start_t, end_t, total_t;
	double time_sending_data, time_computation, time_receiving_data;

	int no_huffman_compression =1;

    cl_platform_id platform_id;         // platform id
    cl_device_id device_id;             // compute device id
    cl_context context;                 // compute context
    cl_command_queue commands;          // compute command queue
    cl_program program;                 // compute programs
    cl_kernel kernel;                   // compute kernel

    cl_uint* h_data;                                // host memory for input vector
    char cl_platform_vendor[1001];
    char target_device_name[1001] = TARGET_DEVICE;

    compressed_file = (BYTE*)aligned_alloc(MEM_ALIGNMENT,MAX_COMPRESSED_SIZE);

    cl_uint* h_axi00_ptr0_output = (cl_uint*)aligned_alloc(MEM_ALIGNMENT,MAX_LENGTH*sizeof(cl_uint*)); // host memory for output vector
    cl_mem d_axi00_ptr0;                         // device memory used for a vector

    cl_uint* fse_streaming_ptr = (cl_uint*)aligned_alloc(MEM_ALIGNMENT,MAX_LENGTH*sizeof(cl_uint*)); // host memory for output vector
    cl_mem d_axi00_ptr1;                         // device memory used for a vector

    cl_uint* huffman_streaming_ptr = (cl_uint*)aligned_alloc(MEM_ALIGNMENT,MAX_LENGTH*sizeof(cl_uint*)); // host memory for output vector
    cl_mem d_axi01_ptr0;                         // device memory used for a vector

    cl_uint* metadata_streaming_ptr = (cl_uint*)aligned_alloc(MEM_ALIGNMENT,MAX_LENGTH*sizeof(cl_uint*)); // host memory for output vector
    cl_mem d_axi02_ptr0;                         // device memory used for a vector

    if (argc != 2) {
        printf("Usage: %s xclbin\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Fill our data sets with pattern
    //h_data = (cl_uint*)aligned_alloc(MEM_ALIGNMENT,MAX_LENGTH * sizeof(cl_uint*));
    for(cl_uint i = 0; i < MAX_LENGTH; i++) {
    	fse_streaming_ptr[i] = 0;
    	huffman_streaming_ptr[i] = 0;
    	metadata_streaming_ptr[i] = 0;
    }
	//get the data in
//	cl_uint rd_size = load_file_to_memory("/home/jianyuchen/benchmark/frix102500K.pcapns", (char **) &h_data);
//	cl_uint rd_size = load_file_to_memory("/home/jianyuchen/benchmark/aucm3.erf", (char **) &h_data);
//	cl_uint rd_size = load_file_to_memory("/home/jianyuchen/benchmark/lnba3.erf", (char **) &h_data);
	cl_uint rd_size = load_file_to_memory("/home/jianyuchen/silesia/test1", (char **) &h_data);
    if (rd_size < 0) {
        printf("failed to read input file\n");
        printf("Test failed\n");
        return EXIT_FAILURE;
    }

    // Get all platforms and then select Xilinx platform
    cl_platform_id platforms[16];       // platform id
    cl_uint platform_count;
    cl_uint platform_found = 0;
    err = clGetPlatformIDs(16, platforms, &platform_count);
    if (err != CL_SUCCESS) {
        printf("Error: Failed to find an OpenCL platform!\n");
        printf("Test failed\n");
        return EXIT_FAILURE;
    }
    printf("INFO: Found %d platforms\n", platform_count);

    // Find Xilinx Plaftorm
    for (cl_uint iplat=0; iplat<platform_count; iplat++) {
        err = clGetPlatformInfo(platforms[iplat], CL_PLATFORM_VENDOR, 1000, (void *)cl_platform_vendor,NULL);
        if (err != CL_SUCCESS) {
            printf("Error: clGetPlatformInfo(CL_PLATFORM_VENDOR) failed!\n");
            printf("Test failed\n");
            return EXIT_FAILURE;
        }
        if (strcmp(cl_platform_vendor, "Xilinx") == 0) {
            printf("INFO: Selected platform %d from %s\n", iplat, cl_platform_vendor);
            platform_id = platforms[iplat];
            platform_found = 1;
        }
    }
    if (!platform_found) {
        printf("ERROR: Platform Xilinx not found. Exit.\n");
        return EXIT_FAILURE;
    }

    // Get Accelerator compute device
    cl_uint num_devices;
    cl_uint device_found = 0;
    cl_device_id devices[16];  // compute device id
    char cl_device_name[1001];
    err = clGetDeviceIDs(platform_id, CL_DEVICE_TYPE_ACCELERATOR, 16, devices, &num_devices);
    printf("INFO: Found %d devices\n", num_devices);
    if (err != CL_SUCCESS) {
        printf("ERROR: Failed to create a device group!\n");
        printf("ERROR: Test failed\n");
        return -1;
    }

    //iterate all devices to select the target device.
    for (cl_uint i=0; i<num_devices; i++) {
        err = clGetDeviceInfo(devices[i], CL_DEVICE_NAME, 1024, cl_device_name, 0);
        if (err != CL_SUCCESS) {
            printf("Error: Failed to get device name for device %d!\n", i);
            printf("Test failed\n");
            return EXIT_FAILURE;
        }
        printf("CL_DEVICE_NAME %s\n", cl_device_name);
        if(strcmp(cl_device_name, target_device_name) == 0) {
            device_id = devices[i];
            device_found = 1;
            printf("Selected %s as the target device\n", cl_device_name);
        }
    }

    if (!device_found) {
        printf("Target device %s not found. Exit.\n", target_device_name);
        return EXIT_FAILURE;
    }

    // Create a compute context
    //
    context = clCreateContext(0, 1, &device_id, NULL, NULL, &err);
    if (!context) {
        printf("Error: Failed to create a compute context!\n");
        printf("Test failed\n");
        return EXIT_FAILURE;
    }

    // Create a command commandsFIFO_OUT_THRESH
    commands = clCreateCommandQueue(context, device_id, CL_QUEUE_PROFILING_ENABLE | CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE, &err);
    if (!commands) {
        printf("Error: Failed to create a command commands!\n");
        printf("Error: code %i\n",err);
        printf("Test failed\n");
        return EXIT_FAILURE;
    }

    cl_int status;

    // Create Program Objects
    // Load binary from disk
    unsigned char *kernelbinary;
    char *xclbin = argv[1];

    //------------------------------------------------------------------------------
    // xclbin
    //------------------------------------------------------------------------------
    printf("INFO: loading xclbin %s\n", xclbin);
    cl_uint n_i0 = load_file_to_memory(xclbin, (char **) &kernelbinary);
    if (n_i0 < 0) {
        printf("failed to load kernel from xclbin: %s\n", xclbin);
        printf("Test failed\n");
        return EXIT_FAILURE;
    }

    size_t n0 = n_i0;

    // Create the compute program from offline
    program = clCreateProgramWithBinary(context, 1, &device_id, &n0,
                                        (const unsigned char **) &kernelbinary, &status, &err);
    free(kernelbinary);

    if ((!program) || (err!=CL_SUCCESS)) {
        printf("Error: Failed to create compute program from binary %d!\n", err);
        printf("Test failed\n");
        return EXIT_FAILURE;
    }


    // Build the program executable
    //
    err = clBuildProgram(program, 0, NULL, NULL, NULL, NULL);
    if (err != CL_SUCCESS) {
        size_t len;
        char buffer[2048];

        printf("Error: Failed to build program executable!\n");
        clGetProgramBuildInfo(program, device_id, CL_PROGRAM_BUILD_LOG, sizeof(buffer), buffer, &len);
        printf("%s\n", buffer);
        printf("Test failed\n");
        return EXIT_FAILURE;
    }

    // Create the compute kernel in the program we wish to run
    //
    kernel = clCreateKernel(program, "rtl_kernel_wizard_0", &err);
    if (!kernel || err != CL_SUCCESS) {
        printf("Error: Failed to create compute kernel!\n");
        printf("Test failed\n");
        return EXIT_FAILURE;
    }

    // Create structs to define memory bank mapping
    cl_mem_ext_ptr_t mem_ext;
    mem_ext.obj = NULL;
    mem_ext.param = kernel;


    mem_ext.flags = 2;
    d_axi00_ptr0 = clCreateBuffer(context,  CL_MEM_READ_WRITE | CL_MEM_EXT_PTR_XILINX,  MAX_LENGTH*sizeof(cl_uint*), &mem_ext, &err);
    if (err != CL_SUCCESS) {
      std::cout << "Return code for clCreateBuffer flags=" << mem_ext.flags << ": " << err << std::endl;
    }


    mem_ext.flags = 3;
    d_axi00_ptr1 = clCreateBuffer(context,  CL_MEM_READ_WRITE | CL_MEM_EXT_PTR_XILINX,  MAX_LENGTH*sizeof(cl_uint*), &mem_ext, &err);
    if (err != CL_SUCCESS) {
      std::cout << "Return code for clCreateBuffer flags=" << mem_ext.flags << ": " << err << std::endl;
    }


    mem_ext.flags = 4;
    d_axi01_ptr0 = clCreateBuffer(context,  CL_MEM_READ_WRITE | CL_MEM_EXT_PTR_XILINX,  MAX_LENGTH*sizeof(cl_uint*), &mem_ext, &err);
    if (err != CL_SUCCESS) {
      std::cout << "Return code for clCreateBuffer flags=" << mem_ext.flags << ": " << err << std::endl;
    }


    mem_ext.flags = 5;
    d_axi02_ptr0 = clCreateBuffer(context,  CL_MEM_READ_WRITE | CL_MEM_EXT_PTR_XILINX,  MAX_LENGTH*sizeof(cl_uint*), &mem_ext, &err);
    if (err != CL_SUCCESS) {
      std::cout << "Return code for clCreateBuffer flags=" << mem_ext.flags << ": " << err << std::endl;
    }


    if (!(d_axi00_ptr0&&d_axi01_ptr0&&d_axi02_ptr0)) {
        printf("Error: Failed to allocate device memory!\n");
        printf("Test failed\n");
        return EXIT_FAILURE;
    }

    start_t = clock();

    err = clEnqueueWriteBuffer(commands, d_axi00_ptr0, CL_TRUE, 0, src_number_of_bytes, h_data, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        printf("Error: Failed to write to source array h_data: d_axi00_ptr0: %d!\n", err);
        printf("Test failed\n");
        return EXIT_FAILURE;
    }
    end_t = clock();
    time_sending_data = (double)(end_t - start_t) / CLOCKS_PER_SEC;

//
//    err = clEnqueueWriteBuffer(commands, d_axi01_ptr0, CL_TRUE, 0, sizeof(cl_uint) * number_of_words, h_data, 0, NULL, NULL);
//    if (err != CL_SUCCESS) {
//        printf("Error: Failed to write to source array h_data: d_axi01_ptr0: %d!\n", err);
//        printf("Test failed\n");
//        return EXIT_FAILURE;
//    }
//
//
//    err = clEnqueueWriteBuffer(commands, d_axi02_ptr0, CL_TRUE, 0, sizeof(cl_uint) * number_of_words, h_data, 0, NULL, NULL);
//    if (err != CL_SUCCESS) {
//        printf("Error: Failed to write to source array h_data: d_axi02_ptr0: %d!\n", err);
//        printf("Test failed\n");
//        return EXIT_FAILURE;
//    }


    // Set the arguments to our compute kernel
    // cl_uint vector_length = MAX_LENGTH;
    err = 0;
    cl_uint d_scalar00 = src_number_of_bytes;
    err |= clSetKernelArg(kernel, 0, sizeof(cl_uint), &d_scalar00); // Not used in example RTL logic.
    cl_uint d_scalar01 = 0;
    err |= clSetKernelArg(kernel, 1, sizeof(cl_uint), &d_scalar01); // Not used in example RTL logic.
    err |= clSetKernelArg(kernel, 2, sizeof(cl_mem), &d_axi00_ptr0); 
    err |= clSetKernelArg(kernel, 3, sizeof(cl_mem), &d_axi00_ptr1); // Not used in example RTL logic.
    err |= clSetKernelArg(kernel, 4, sizeof(cl_mem), &d_axi01_ptr0); 
    err |= clSetKernelArg(kernel, 5, sizeof(cl_mem), &d_axi02_ptr0); 

    if (err != CL_SUCCESS) {
        printf("Error: Failed to set kernel arguments! %d\n", err);
        printf("Test failed\n");
        return EXIT_FAILURE;
    }

    size_t global[1];
    size_t local[1];
    // Execute the kernel over the entire range of our 1d input data set
    // using the maximum number of work group items for this device


/************first execution***************/
    global[0] = 1;
    local[0] = 1;

    start_t = clock();

 //   cl_event compression_event;
    err = clEnqueueNDRangeKernel(commands, kernel, 1, NULL, (size_t*)&global, (size_t*)&local, 0, NULL, NULL);
    if (err) {
        printf("Error: Failed to execute kernel! %d\n", err);
        printf("Test failed\n");
        return EXIT_FAILURE;
    }
    clFinish(commands);
//    clWaitForEvents(1, &compression_event);
    end_t = clock();
    time_computation = (double)(end_t - start_t) / (double)CLOCKS_PER_SEC;

/************second execution***************/
//        global[0] = 1;
//        local[0] = 1;
//        err = clEnqueueNDRangeKernel(commands, kernel, 1, NULL, (size_t*)&global, (size_t*)&local, 0, NULL, NULL);
//        if (err) {
//            printf("Error: Failed to execute kernel! %d\n", err);
//            printf("Test failed\n");
//            return EXIT_FAILURE;
//        }
//
//        clFinish(commands);


    // Read back the results from the device to verify the output
    //
    cl_event readevent;

    start_t = clock();

/********** read metadata from host**************/
    err |= clEnqueueReadBuffer( commands, d_axi00_ptr1, CL_TRUE, 0, estimated_number_of_block*64, metadata_streaming_ptr, 0, NULL, &readevent );
    if (err != CL_SUCCESS) {
        printf("Error: Failed to read output array! %d\n", err);
        printf("Test failed\n");
        return EXIT_FAILURE;
    }
    clWaitForEvents(1, &readevent);
    block_num = *(((unsigned*)metadata_streaming_ptr)+10);
    if(block_num > MAX_METADATA){
    	printf("Too many blocks\n");
    	return EXIT_FAILURE;
    }
    printf("Number of block: %d \n", block_num);
    int mem_4kblock_num = block_num/64 + (block_num%64 != 0);

    if(mem_4kblock_num > estimated_number_of_block/64){ //if the number of metadata is large, read again
    	printf("second meta data reading\n");
        err |= clEnqueueReadBuffer( commands, d_axi00_ptr1, CL_TRUE, 0, mem_4kblock_num*4096, metadata_streaming_ptr, 0, NULL, &readevent );
        if (err != CL_SUCCESS) {
            printf("Error: Failed to read output array! %d\n", err);
            printf("Test failed\n");
            return EXIT_FAILURE;
        }
        memcpy(((BYTE*)metadata_streaming_ptr)+mem_4kblock_num*4096, metadata_streaming_ptr, 4096);
    }
    clWaitForEvents(1, &readevent);

 /********** parse the metadata**************/
    if(mem_4kblock_num > estimated_number_of_block/64){
    	parse_metadata(metadatas, (cl_uint*)(((BYTE*)metadata_streaming_ptr)+4096), block_num, no_huffman_compression, &fse_stream_num_bytes, &huffman_stream_num_bytes);
    }else{
    	parse_metadata(metadatas, metadata_streaming_ptr, block_num, no_huffman_compression, &fse_stream_num_bytes, &huffman_stream_num_bytes);
    }

 /**********************************************/
    err = 0;
    err |= clEnqueueReadBuffer( commands, d_axi01_ptr0, CL_TRUE, 0, fse_stream_num_bytes, fse_streaming_ptr, 0, NULL, &readevent );

    err |= clEnqueueReadBuffer( commands, d_axi02_ptr0, CL_TRUE, 0, huffman_stream_num_bytes, huffman_streaming_ptr, 0, NULL, &readevent );


    if (err != CL_SUCCESS) {
        printf("Error: Failed to read output array! %d\n", err);
        printf("Test failed\n");
        return EXIT_FAILURE;
    }
    clWaitForEvents(1, &readevent);

    end_t = clock();
    time_receiving_data = (double)(end_t - start_t) / CLOCKS_PER_SEC;


/***********************start compression**************************/
    int size_added;
    BYTE* pos = compressed_file;
    size_added = format_zstd_stream(metadatas, pos, block_num, no_huffman_compression, fse_streaming_ptr, huffman_streaming_ptr);
    printf("Size of compressed file is %d bytes", size_added);

/******************************************************************/
    int n= write_file_from_memory("/home/jianyuchen/benchmark/hw_result.zst", (char*)compressed_file, size_added);

    printf("%d bytes are written to the file\n", n);
    //--------------------------------------------------------------------------
    // Shutdown and cleanup
    //-------------------------------------------------------------------------- 
    clReleaseMemObject(d_axi00_ptr0);
    free(h_axi00_ptr0_output);

    clReleaseMemObject(d_axi00_ptr1);
    free(fse_streaming_ptr);

    clReleaseMemObject(d_axi01_ptr0);
    free(huffman_streaming_ptr);

    clReleaseMemObject(d_axi02_ptr0);
    free(metadata_streaming_ptr);

    free(compressed_file);

    free(h_data);
    clReleaseProgram(program);
    clReleaseKernel(kernel);
    clReleaseCommandQueue(commands);
    clReleaseContext(context);

    printf("Time of sending data: %lf s time of computation: %lf s, time of receiving data: %lf s \n", time_sending_data, time_computation, time_receiving_data);

    if (check_status) {
        printf("INFO: Test failed\n");
        return EXIT_FAILURE;
    } else {
        printf("INFO: Test completed successfully.\n");
        return EXIT_SUCCESS;
    }



} // end of main

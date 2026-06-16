#include <cstdint>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PAGE_SIZE 4096

#define kLittleEndian (1)

const uint64_t kBlockBasedTableMagicNumber = 0x88e241b785f4cff7ull;

enum ChecksumType : char {
  kNoChecksum = 0x0,
  kCRC32c = 0x1,
  kxxHash = 0x2,
  kxxHash64 = 0x3,
  kXXH3 = 0x4,  // Supported since RocksDB 6.27
};

char* GetVarint64Ptr(char *p, char *limit, uint64_t *value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    uint64_t byte = *(uint8_t*)p;
    p++;
    if (byte & 128) {
      // More bytes are present
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return (char*) p;
    }
  }
  return NULL;
}

char* GetVarint32Ptr(char *p, char *limit, uint32_t *value) {
  if (p < limit) {
    uint32_t result = *(uint8_t*) p;
    if ((result & 128) == 0) {
      *value = result;
      return p + 1;
    }
  }

  // GetVarint32PtrFallback
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    uint32_t byte = *(uint8_t*)p;
    p++;
    if (byte & 128) {
      // More bytes are present
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return (char*) p;
    }
  }
  return NULL;
}

char* DecodeEntry(char* p, char* limit, uint32_t* shared, uint32_t* non_shared, uint32_t* value_length) {
  if (limit - p < 3)
    return NULL;
  *shared = ((uint8_t*)p)[0];
  *non_shared = ((uint8_t*)p)[1];
  *value_length = ((uint8_t*)p)[2];
  if ((*shared | *non_shared | *value_length) < 128) {
    // Fast path: all three values are encoded in one byte each
    p += 3;
  } else {
    if ((p = GetVarint32Ptr(p, limit, shared)) == NULL) return NULL;
    if ((p = GetVarint32Ptr(p, limit, non_shared)) == NULL) return NULL;
    if ((p = GetVarint32Ptr(p, limit, value_length)) == NULL) return NULL;
  }

  if ((limit - p) < (*non_shared + *value_length)) {
    return NULL;
  }
  return p;
}

inline uint32_t DecodeFixed32(const char* ptr) {
  const uint8_t* const buffer = (const uint8_t*)ptr;
  if (kLittleEndian) {
    // Load the raw bytes
    uint32_t result;
    memcpy(&result, ptr, sizeof(result));  // gcc optimizes this to a plain load
    return result;
  } else {
    // Recent clang and gcc optimize this to a single mov / ldr instruction.
    return ((uint32_t)(buffer[0])) |
          ((uint32_t)(buffer[1]) << 8) |
          ((uint32_t)(buffer[2]) << 16) |
          ((uint32_t)(buffer[3]) << 24);
  }
}

inline uint64_t DecodeFixed64(const char* ptr) {
  if (kLittleEndian) {
    // Load the raw bytes
    uint64_t result;
    memcpy(&result, ptr, sizeof(result));  // gcc optimizes this to a plain load
    return result;
  } else {
    uint64_t lo = DecodeFixed32(ptr);
    uint64_t hi = DecodeFixed32(ptr + 4);
    return (hi << 32) | lo;
  }
}

int main(int argc, char ** argv) {
  int fd;
  char * ptr;
  int real_size = 129 * 1024 * 1024;
  //int real_size = 3175972;
  int kNewVersionsEncodedLength = 53;
  int kBlockTrailerSize = 5;
  int block_size = 4096;
  int cumulative_size = 0;

  fd = open(argv[1], O_RDWR | O_SYNC);
//fd = open("/mnt/nvme/rocks/000248.ldb", O_RDWR | O_SYNC);
  if (fd == -1)
    printf("Error open\n");

  printf("[read] buffer size: %dKB\n", real_size/1024);

  ptr = (char *)memalign(PAGE_SIZE, real_size);

  int ret = pread(fd, ptr, real_size, 0);
  if (ret <= 0) {
    printf("PREAD ERROR[%d] (%d, %d)\n", errno, ret, real_size);
  } else {
    printf("PREAD SUCCESS %d\n", ret);
	  real_size = ret;
  }
  printf("real size: %u\n", real_size);

  /* Decode footer */
  char footer_space[kNewVersionsEncodedLength];
  memcpy(footer_space, ptr + (real_size - kNewVersionsEncodedLength), kNewVersionsEncodedLength);
  uint64_t val[4];

  char* magic_ptr = footer_space + kNewVersionsEncodedLength - 8;
  uint64_t magic = DecodeFixed64(magic_ptr);

  if (magic != kBlockBasedTableMagicNumber) {
    printf("Corruption at magic number!!\n");
    // return 0;
  }

  // Parse Part3 (format must be checked first)
  const char* part3_ptr = magic_ptr - 4;
  uint32_t computed_checksum = 0;
  uint64_t footer_offset = 0;

  uint32_t format_version = DecodeFixed32(part3_ptr);
  printf("format version: %d\n", format_version);

  // Parse Part1
  char chksum = footer_space[0];
  int checksum_type = (ChecksumType)chksum;
  printf("checksum type: %d\n", checksum_type);

  // Parse Part2
  int leftover_size = kNewVersionsEncodedLength - 1;
  char* q = footer_space + 1;
  for (int i = 0; i < 4; i++) {
    char* limit = q + leftover_size;
    q = GetVarint64Ptr(q, limit, &val[i]);
    leftover_size = (limit - q);
  }
  uint64_t tail_start_offset = val[2];
  printf("%lu %lu %lu %lu\n", val[0], val[1], val[2], val[3]);
  printf("%s - datablock: %lu\n", argv[1], tail_start_offset);

#if 0
  /*Get block block data */
  /* Base on the notion that we know we can skip the BlockTrailer */
  size_t offset = 0;
  size_t final_offset = tail_start_offset;
  char* p = ptr;
  char* limit = p + final_offset;
  char* prev;
  while (p < limit) {
    uint32_t shared;
    uint32_t non_shared;
    uint32_t value_length;
    char key[30];
    char value[5000];
    memset(key, 0, 30);

    if ((limit - p) == (8 + kBlockTrailerSize)) {
      break; // end of block
    }
    
    prev = p;
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    memcpy(key, p, non_shared);
    p += non_shared;
    memcpy(value, p, value_length);
    p += value_length;
//	printf("key: %s, shared: %d, non_shared: %d, value size: %llu, %d\n", key, shared, non_shared, value_length, (p - ptr));
    printf("key: %s, shared: %d, non_shared: %d, value size: %u\n", key, shared, non_shared, value_length);

    cumulative_size += (p - prev);

    if (cumulative_size >= block_size) {
      cumulative_size = 0;
      p += 8; // magic number for restart
      p += kBlockTrailerSize;
    }
  }
#endif

#if 0
  /* Get index block data */
  size_t offset = 0;
  size_t final_offset = tail_start_offset;
  char* p = ptr + val[2];
  char* limit = p + val[3];
  uint64_t value_offset = 0, value_size = 0;

  while ((value_offset + value_size + 5) < final_offset) {
    uint32_t shared;
    uint32_t non_shared;
    uint32_t value_length;
    char key[20], value[20];
    memset(key, 0, 20);

    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    memcpy(key, p, non_shared);
    p += non_shared;
    memcpy(value, p, value_length);
    p += value_length;

    printf("non_shared: %d, value_length: %d\n", non_shared, value_length);
    char *value_p = value;
    char* value_limit = value + value_length;
    value_p = GetVarint64Ptr(value_p, value_limit, &value_offset);
    value_p = GetVarint64Ptr(value_p, value_limit, &value_size);
    printf("key: %s, value offset:%llu, value size: %llu, %d\n", key, value_offset, value_size, (limit - p));
  }
#endif

#if 0
  /* Get properties block data */
  q = ptr + val[0];
  char* q_limit = ptr + val[0] + val[1];
  uint64_t properties_offset, properties_size;
  uint32_t prop_shared, prop_non_shared, pop_value_length;
  q = DecodeEntry(q, q_limit, &prop_shared, &prop_non_shared, &pop_value_length);
  q += prop_non_shared;
  q = GetVarint64Ptr(q, q_limit, &properties_offset);
  q = GetVarint64Ptr(q, q_limit, &properties_size);

  size_t offset = 0;
  char* prop = ptr + properties_offset;
  char* prop_limit = prop + (properties_size - 8); // remove restarts_ and num_restarts
  int counter = 0;
  char last_key[50];
  while (prop < prop_limit) {
    uint32_t shared;
    uint32_t non_shared;
    uint32_t value_length;
    char key[30];
    char value[5000];
    memset(key, 0, 30);
    memset(value, 0, 5000);

    prop = DecodeEntry(prop, prop_limit, &shared, &non_shared, &value_length);
    memcpy(key, prop, non_shared);
    prop += non_shared;
    memcpy(value, prop, value_length);
    prop += value_length;

    if (counter == 8 || counter == 9) {
      printf("%d - key: %s, value: %s\n", counter, key, value);
    } else if (counter == 10 || counter == 13 || counter == 26) {
      char* val_ptr = value;
      uint64_t val;
      GetVarint64Ptr(val_ptr, val_ptr + value_length, &val);
      printf("%d - key: %s, value: %lu\n", counter, key, val);
    } else {
      printf("%d - key: %s\n", counter, key);
    }

    counter++;
  }


#endif

  close(fd);

  return 0;
}



#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>

#define PAGE_TABLE_SIZE 256
#define PAGE_SIZE       256
#define FRAME_SIZE      256
#define NUM_FRAMES      128
#define TLB_SIZE        16
#define OFFSET_MASK     0xFF
#define OFFSET_BITS     8
#define BACKING_STORE_FILE "BACKING_STORE.bin"
#define OUTPUT_FILE        "correct.txt"

typedef enum {
    ALG_FIFO,
    ALG_LRU
} ReplacementAlg;

typedef struct {
    int page_number;
    int frame_number;
    bool valid;
    unsigned long last_used;
} TLBEntry;

typedef struct {
    int frame_number;
    bool valid;
} PageTableEntry;

typedef struct {
    int page_number;
    bool occupied;
    unsigned long last_used;
} FrameInfo;

typedef struct {
    int index;
    int target_page;
} TLBSearchArg;


TLBEntry tlb[TLB_SIZE];
PageTableEntry page_table[PAGE_TABLE_SIZE];
FrameInfo frame_table[NUM_FRAMES];
signed char physical_memory[NUM_FRAMES][FRAME_SIZE];


int shared_tlb_hit_index = -1;
pthread_mutex_t tlb_mutex = PTHREAD_MUTEX_INITIALIZER;


int next_free_frame = 0;
int fifo_frame_ptr = 0;
int tlb_count = 0;
int fifo_tlb_ptr = 0;
unsigned long global_time = 0;


void *search_tlb_entry(void *arg) {
    TLBSearchArg *search_arg = (TLBSearchArg *)arg;
    int idx = search_arg->index;

    if (tlb[idx].valid && tlb[idx].page_number == search_arg->target_page) {
        pthread_mutex_lock(&tlb_mutex);
        if (shared_tlb_hit_index == -1) {
            shared_tlb_hit_index = idx;
        }
        pthread_mutex_unlock(&tlb_mutex);
    }
    return NULL;
}


int lookup_tlb_parallel(int page_number, int *frame_number) {
    pthread_t threads[TLB_SIZE];
    TLBSearchArg args[TLB_SIZE];

    shared_tlb_hit_index = -1;

    for (int i = 0; i < TLB_SIZE; i++) {
        args[i].index = i;
        args[i].target_page = page_number;
        if (pthread_create(&threads[i], NULL, search_tlb_entry, &args[i]) != 0) {
            fprintf(stderr, "Erro fatal: falha ao criar thread para busca na TLB.\n");
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < TLB_SIZE; i++) {
        pthread_join(threads[i], NULL);
    }

    if (shared_tlb_hit_index != -1) {
        *frame_number = tlb[shared_tlb_hit_index].frame_number;
        tlb[shared_tlb_hit_index].last_used = ++global_time;
        return shared_tlb_hit_index;
    }

    return -1;
}

int update_tlb(int page_number, int frame_number, ReplacementAlg tlb_alg) {
    int target_idx = -1;

    for (int i = 0; i < TLB_SIZE; i++) {
        if (tlb[i].valid && tlb[i].page_number == page_number) {
            target_idx = i;
            break;
        }
    }

    if (target_idx == -1) {
        if (tlb_count < TLB_SIZE) {
            target_idx = tlb_count;
            tlb_count++;
        } else if (tlb_alg == ALG_FIFO) {
            target_idx = fifo_tlb_ptr;
            fifo_tlb_ptr = (fifo_tlb_ptr + 1) % TLB_SIZE;
        } else { 
            unsigned long min_time = ULONG_MAX;
            for (int i = 0; i < TLB_SIZE; i++) {
                if (tlb[i].last_used < min_time) {
                    min_time = tlb[i].last_used;
                    target_idx = i;
                }
            }
        }
    }

    tlb[target_idx].page_number = page_number;
    tlb[target_idx].frame_number = frame_number;
    tlb[target_idx].valid = true;
    tlb[target_idx].last_used = ++global_time;

    return target_idx;
}


void invalidate_tlb_page(int evicted_page) {
    for (int i = 0; i < TLB_SIZE; i++) {
        if (tlb[i].valid && tlb[i].page_number == evicted_page) {
            tlb[i].valid = false;
        }
    }
}

int handle_page_fault(int page_number, FILE *backing_store, ReplacementAlg page_alg) {
    int target_frame = -1;

    if (next_free_frame < NUM_FRAMES) {
        target_frame = next_free_frame++;
    } else {
        if (page_alg == ALG_FIFO) {
            target_frame = fifo_frame_ptr;
            fifo_frame_ptr = (fifo_frame_ptr + 1) % NUM_FRAMES;
        } else { 
            unsigned long min_time = ULONG_MAX;
            for (int i = 0; i < NUM_FRAMES; i++) {
                if (frame_table[i].last_used < min_time) {
                    min_time = frame_table[i].last_used;
                    target_frame = i;
                }
            }
        }

        
        int old_page = frame_table[target_frame].page_number;
        page_table[old_page].valid = false;
        invalidate_tlb_page(old_page);
    }

    if (fseek(backing_store, page_number * PAGE_SIZE, SEEK_SET) != 0) {
        fprintf(stderr, "Erro de E/S: falha ao posicionar ponteiro no %s.\n", BACKING_STORE_FILE);
        exit(EXIT_FAILURE);
    }

    size_t bytes_read = fread(physical_memory[target_frame], sizeof(signed char), FRAME_SIZE, backing_store);
    if (bytes_read != FRAME_SIZE) {
        fprintf(stderr, "Erro de E/S: falha ao ler página %d do %s.\n", page_number, BACKING_STORE_FILE);
        exit(EXIT_FAILURE);
    }

    page_table[page_number].frame_number = target_frame;
    page_table[page_number].valid = true;

    frame_table[target_frame].page_number = page_number;
    frame_table[target_frame].occupied = true;
    frame_table[target_frame].last_used = ++global_time;

    return target_frame;
}


bool parse_algorithm(const char *arg, ReplacementAlg *alg) {
    if (strcmp(arg, "fifo") == 0) {
        *alg = ALG_FIFO;
        return true;
    }
    if (strcmp(arg, "lru") == 0) {
        *alg = ALG_LRU;
        return true;
    }
    return false;
}

int *load_and_validate_addresses(const char *filename, size_t *count) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Erro: não foi possível abrir o arquivo de endereços '%s' (%s).\n",
                filename, strerror(errno));
        return NULL;
    }

    size_t capacity = 1024;
    size_t size = 0;
    int *addresses = malloc(capacity * sizeof(int));
    if (!addresses) {
        fprintf(stderr, "Erro: falha de alocação de memória.\n");
        fclose(fp);
        return NULL;
    }

    char line[256];
    size_t line_num = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        line_num++;


        char *start = line;
        while (isspace((unsigned char)*start)) {
            start++;
        }

        if (*start == '\0') {
            continue;
        }

        char *endptr = NULL;
        errno = 0;
        long val = strtol(start, &endptr, 10);


        while (isspace((unsigned char)*endptr)) {
            endptr++;
        }

        if (errno != 0 || *endptr != '\0' || val < 0 || val > 65535) {
            fprintf(stderr, "Erro de formatação na linha %zu de '%s': esperado inteiro entre 0 e 65535, encontrado '%s'.\n",
                    line_num, filename, start);
            free(addresses);
            fclose(fp);
            return NULL;
        }

        if (size == capacity) {
            capacity *= 2;
            int *tmp = realloc(addresses, capacity * sizeof(int));
            if (!tmp) {
                fprintf(stderr, "Erro: falha ao realocar memória.\n");
                free(addresses);
                fclose(fp);
                return NULL;
            }
            addresses = tmp;
        }

        addresses[size++] = (int)val;
    }

    fclose(fp);

    if (size == 0) {
        fprintf(stderr, "Erro: o arquivo '%s' está vazio ou não contém endereços válidos.\n", filename);
        free(addresses);
        return NULL;
    }

    *count = size;
    return addresses;
}

int main(int argc, char *argv[]) {

    if (argc != 4) {
        fprintf(stderr, "Erro: número inválido de argumentos.\n");
        fprintf(stderr, "Uso correto: %s <arquivo_enderecos> <fifo|lru> <fifo|lru>\n", argv[0]);
        return EXIT_FAILURE;
    }


    ReplacementAlg page_alg, tlb_alg;
    if (!parse_algorithm(argv[2], &page_alg)) {
        fprintf(stderr, "Erro: algoritmo de substituição de página inválido ('%s'). Use 'fifo' ou 'lru'.\n", argv[2]);
        return EXIT_FAILURE;
    }

    if (!parse_algorithm(argv[3], &tlb_alg)) {
        fprintf(stderr, "Erro: algoritmo de substituição da TLB inválido ('%s'). Use 'fifo' ou 'lru'.\n", argv[3]);
        return EXIT_FAILURE;
    }


    size_t total_addresses = 0;
    int *addresses = load_and_validate_addresses(argv[1], &total_addresses);
    if (!addresses) {
        return EXIT_FAILURE;
    }

    FILE *backing_store = fopen(BACKING_STORE_FILE, "rb");
    if (!backing_store) {
        fprintf(stderr, "Erro: não foi possível abrir '%s' (%s).\n",
                BACKING_STORE_FILE, strerror(errno));
        free(addresses);
        return EXIT_FAILURE;
    }

    fseek(backing_store, 0, SEEK_END);
    long store_size = ftell(backing_store);
    if (store_size < PAGE_TABLE_SIZE * PAGE_SIZE) {
        fprintf(stderr, "Erro: arquivo '%s' corrompido ou menor que 65536 bytes.\n", BACKING_STORE_FILE);
        fclose(backing_store);
        free(addresses);
        return EXIT_FAILURE;
    }
    rewind(backing_store);

    FILE *output = fopen(OUTPUT_FILE, "w");
    if (!output) {
        fprintf(stderr, "Erro: não foi possível criar o arquivo de saída '%s' (%s).\n",
                OUTPUT_FILE, strerror(errno));
        fclose(backing_store);
        free(addresses);
        return EXIT_FAILURE;
    }


    memset(tlb, 0, sizeof(tlb));
    memset(page_table, 0, sizeof(page_table));
    memset(frame_table, 0, sizeof(frame_table));

    size_t page_faults = 0;
    size_t tlb_hits = 0;

    for (size_t i = 0; i < total_addresses; i++) {
        int logical_addr = addresses[i];
        int page_number = (logical_addr >> OFFSET_BITS) & OFFSET_MASK;
        int offset = logical_addr & OFFSET_MASK;

        int frame_number = -1;
        int tlb_index = lookup_tlb_parallel(page_number, &frame_number);

        if (tlb_index != -1) {

            tlb_hits++;
            frame_table[frame_number].last_used = ++global_time;
        } else {
 
            if (page_table[page_number].valid) {
                frame_number = page_table[page_number].frame_number;
                frame_table[frame_number].last_used = ++global_time;
            } else {

                page_faults++;
                frame_number = handle_page_fault(page_number, backing_store, page_alg);
            }
 
            tlb_index = update_tlb(page_number, frame_number, tlb_alg);
        }

        int physical_addr = (frame_number << OFFSET_BITS) | offset;
        signed char value = physical_memory[frame_number][offset];

        fprintf(output, "Virtual address: %d TLB: %d Physical address: %d Value: %d\n",
                logical_addr, tlb_index, physical_addr, value);
    }

    double fault_rate = ((double)page_faults / (double)total_addresses) * 100.0;
    double tlb_rate = ((double)tlb_hits / (double)total_addresses) * 100.0;

    fprintf(output, "Number of Translated Addresses = %zu\n", total_addresses);
    fprintf(output, "Page Faults = %zu\n", page_faults);
    fprintf(output, "Page Fault Rate = %.3f%%\n", fault_rate);
    fprintf(output, "TLB Hits = %zu\n", tlb_hits);
    fprintf(output, "TLB Hit Rate = %.3f%%\n", tlb_rate);


    fclose(output);
    fclose(backing_store);
    free(addresses);
    pthread_mutex_destroy(&tlb_mutex);

    return EXIT_SUCCESS;
}

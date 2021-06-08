#include "gr-gate.h"
#include "virtual_memory.h" // Memory allocation.
#include <unistd.h>         // usleep

// Only 3 CN algos are selected from available 6.
__thread uint8_t gr_hash_order[GR_HASH_FUNC_COUNT - 3 + 1];

#if defined(GR_4WAY)

__thread gr_4way_context_overlay gr_4way_ctx;

#endif

__thread gr_context_overlay gr_ctx;

__thread uint8_t *hp_state = NULL;

bool register_gr_algo(algo_gate_t *gate) {
#if defined(GR_4WAY)
  gate->scanhash = (void *)&scanhash_gr_4way;
  gate->hash = (void *)&gr_4way_hash;
#else
  gate->scanhash = (void *)&scanhash_gr;
  gate->hash = (void *)&gr_hash;
#endif
  gate->optimizations = SSE2_OPT | AES_OPT | VAES_OPT | AVX2_OPT;
  opt_target_factor = 65536.0;
  return true;
}

static void selectAlgo(const uint8_t nibble, bool *selectedAlgos,
                       uint8_t *selectedIndex, int algoCount,
                       int *currentCount) {
  uint8_t algoDigit = (nibble & 0x0F) % algoCount;
  if (!selectedAlgos[algoDigit]) {
    selectedAlgos[algoDigit] = true;
    selectedIndex[currentCount[0]] = algoDigit;
    currentCount[0] = currentCount[0] + 1;
  }
  algoDigit = (nibble >> 4) % algoCount;
  if (!selectedAlgos[algoDigit]) {
    selectedAlgos[algoDigit] = true;
    selectedIndex[currentCount[0]] = algoDigit;
    currentCount[0] = currentCount[0] + 1;
  }
}

void gr_getAlgoString(const uint8_t *block, uint8_t *selectedAlgoOutput) {
  // Select Core algos.
  bool selectedCoreAlgo[15];

  for (int i = 0; i < 15; i++) {
    selectedCoreAlgo[i] = false;
  }

  uint8_t core_algos[15];
  int selectedCoreCount = 0;
  for (int i = 0; i < 32; i++) {
    selectAlgo(block[i], selectedCoreAlgo, core_algos, 15, &selectedCoreCount);
    if (selectedCoreCount == 15) {
      break;
    }
  }
  if (selectedCoreCount < 15) {
    for (int i = 0; i < 15; i++) {
      if (!selectedCoreAlgo[i]) {
        core_algos[selectedCoreCount] = i;
        selectedCoreCount++;
      }
    }
  }

  // Select Core algos.
  bool selectedCNAlgo[6];

  for (int i = 0; i < 6; i++) {
    selectedCNAlgo[i] = false;
  }

  uint8_t cn_algos[6];
  int selectedCNCount = 0;
  for (int i = 0; i < 32; i++) {
    selectAlgo(block[i], selectedCNAlgo, cn_algos, 6, &selectedCNCount);
    if (selectedCNCount == 6) {
      break;
    }
  }
  if (selectedCNCount < 6) {
    for (int i = 0; i < 6; i++) {
      if (!selectedCNAlgo[i]) {
        cn_algos[selectedCNCount] = i;
        selectedCNCount++;
      }
    }
  }

  selectedCNCount = 0;
  selectedCoreCount = 0;
  // Create proper algo order.
  for (int i = 0; i < 15 + 3; i++) {
    if (i % 6 == 5) {
      // Add CN algo.
      selectedAlgoOutput[i] = cn_algos[selectedCNCount++] + 15;
      i++;
      if (i == 18) {
        break;
      }
    }
    selectedAlgoOutput[i] = core_algos[selectedCoreCount++];
  }
}

// Mapping of gr_harh_order CN to cn-config - lightest to heaviest order.
// Config:  Turtlelite, Turtle, Darklite, Dark, Lite, Fast.
// Gr_Hash: Dark, Darklite, Fast, Lite, Turtle, Turtlelite
static uint8_t cn_map[6] = {3, 2, 5, 4, 1, 0};

static size_t GetMaxCnSize() {
  // Memory requirements for each CN variant
  size_t cn_req[6] = {262144, 262144, 524288, 524288, 1048576, 2097152};
  // Check tune/config if given variant uses 2way that requires 2x memory.
  // cn_config should contain only 0 values in non GR_4WAY.
  for (int i = 0; i < 6; i++) {
    cn_req[i] *= (cn_config[i] + 1);
  }

  size_t order[3] = {cn_map[gr_hash_order[5] - 15],
                     cn_map[gr_hash_order[11] - 15],
                     cn_map[gr_hash_order[17] - 15]};
  size_t max =
      cn_req[order[0]] > cn_req[order[1]] ? cn_req[order[0]] : cn_req[order[1]];
  max = max > cn_req[order[2]] ? max : cn_req[order[2]];

  return max;
}

void AllocateNeededMemory() {
  size_t size = GetMaxCnSize();

  // Purges previous memory allocation and creates new one.
  PrepareMemory((void **)&hp_state, size);
}

void select_tuned_config(int thr_id) {
  for (size_t i = 0; i < 20; i++) {
    if (cn[i][0] + 15 == gr_hash_order[5] ||
        cn[i][0] + 15 == gr_hash_order[11] ||
        cn[i][0] + 15 == gr_hash_order[17]) {
      if (cn[i][1] + 15 == gr_hash_order[5] ||
          cn[i][1] + 15 == gr_hash_order[11] ||
          cn[i][1] + 15 == gr_hash_order[17]) {
        if (cn[i][2] + 15 == gr_hash_order[5] ||
            cn[i][2] + 15 == gr_hash_order[11] ||
            cn[i][2] + 15 == gr_hash_order[17]) {
          memcpy(cn_config, &cn_tune[i], 6);
          if (opt_debug && !thr_id) {
            applog(LOG_BLUE, "config %d: %d %d %d %d %d %d", i, cn_config[0],
                   cn_config[1], cn_config[2], cn_config[3], cn_config[4],
                   cn_config[5]);
          }
          return;
        }
      }
    }
  }
  if (!thr_id) {
    // Should not get to this point.
    applog(LOG_ERR, "Could not find any config? %d %d %d", gr_hash_order[5],
           gr_hash_order[11], gr_hash_order[17]);
  }
  return;
}

static double bench_time = 0.0;
static double bench_hashes = 0.0;
static double bench_hashrate = 0.0;
static uint8_t rotation = 0;
static volatile bool stop_benchmark = false;

static void print_stats(const char *prefix, bool same_line) {
  double hashrate;
  char hr_units[4] = {0};

  // lock is not necessary.
  hashrate = bench_hashes / bench_time;
  bench_hashrate = hashrate;
  // scale_hash_for_display(&hashrate, hr_units);
  if (same_line) {
    pthread_mutex_unlock(&applog_lock);
    printf("                      %s\t%.2lf %sH/s (%.2lfs)\t-> %.3lf %sH/s per "
           "thread.\r",
           prefix, hashrate, hr_units, bench_time, hashrate / opt_n_threads,
           hr_units);
    fflush(stdout);
    pthread_mutex_unlock(&applog_lock);

  } else {
    applog(LOG_BLUE, "%s \t%.2lf %sH/s (%.2lfs)\t-> %.3lf %sH/s per thread.",
           prefix, hashrate, hr_units, bench_time, hashrate / opt_n_threads,
           hr_units);
  }
}

static void sync_lock(const int locks) {
  static volatile int done = 0;

  pthread_mutex_lock(&stats_lock);
  done++;
  if (done != locks) {
    pthread_cond_wait(&sync_cond, &stats_lock);
  } else {
    done = 0;
    pthread_cond_broadcast(&sync_cond);
  }
  pthread_mutex_unlock(&stats_lock);
}

static void sync_conf() { sync_lock(opt_n_threads); }

static void sync_bench() { sync_lock(opt_n_threads + 1); }

// Detached thread for changing rotation every 1.5s.
// Prints data every rotation.
void *statistic_thread(void *arg) {
  struct timeval start, end, diff;
  double elapsed;
  sync_bench(); // Sync before benchmark starts.
  sync_bench(); // Rotation change sync.
  while (true) {
    gettimeofday(&start, NULL);
    if (arg != NULL) {
      // Sleep for predefined time period.
      usleep(*((long *)arg));
    } else {
      // Sleep portion of the whole benchmark. Portion is deretmined from
      // real world data of each rotation time mining.
      // This should provide more of a real world average that users can see
      // on the pool side.
      // Total benchmark time is 300s.
      usleep(300000000. * time_ratio[rotation]);
    }
    // Change rotation.
    rotation = (rotation + 1) % 20;
    gettimeofday(&end, NULL);
    timeval_subtract(&diff, &end, &start);
    elapsed = (double)diff.tv_sec + (double)diff.tv_usec / 1e6;
    bench_time += elapsed;
    sync_bench(); // Rotation change sync.
    if (rotation == 0) {
      // Print permanently after full 20 rotations.
      print_stats("Hashrate (Avg): ", false);
      // Make sure it is true before other threads sync.
      stop_benchmark = true;
      sync_bench(); // Config change sync.
      return NULL;
    } else {
      print_stats("Hashrate (Avg): ", true);
    }
  }
}

static void tune_config(void *input, int thr_id, int rot) {
  srand(thr_id);
  rotation = 19;
  long sleep_time = 12500000;
  pthread_t pthr;
  if (thr_id == 0) {
    pthread_create(&pthr, NULL, &statistic_thread, &sleep_time);
  }

  uint32_t n = 10000 * thr_id;
  uint32_t edata0[20] __attribute__((aligned(64)));
  mm128_bswap32_80(edata0, input);
  edata0[19] = n;
#if defined(GR_4WAY)
  uint32_t hash[8 * 4] __attribute__((aligned(64)));
  uint32_t vdata[20 * 4] __attribute__((aligned(64)));
  __m256i *noncev = (__m256i *)vdata + 9; // aligned
  mm256_bswap32_intrlv80_4x64(vdata, input);
  *noncev = mm256_intrlv_blend_32(
      _mm256_set_epi32(n + 3, 0, n + 2, 0, n + 1, 0, n, 0), *noncev);
#else
  uint32_t hash[8 * 2] __attribute__((aligned(64)));
  uint32_t edata1[20] __attribute__((aligned(64)));
  mm128_bswap32_80(edata1, input);
  edata1[19] = n + 1;
#endif
  double hashes_done = 0.0;
  // Use CN rotation.
  edata0[1] = rand();
  edata0[2] = rand();
  gr_getAlgoString((const uint8_t *)(&edata0[1]), gr_hash_order);
  gr_hash_order[5] = cn[rot][0] + 15;
  gr_hash_order[11] = cn[rot][1] + 15;
  gr_hash_order[17] = cn[rot][2] + 15;

  // Purge memory for test.
  AllocateNeededMemory();

  // Set desired CN config.
  sync_bench();
  sync_bench();
  while (true) {
#if defined(GR_4WAY)
    // Make sure nonces are increased for each hash. Same hashes will result
    // in better data locality on CN algos leading to better/innaccurate
    // results.
    gr_4way_hash(hash, vdata, thr_id);
    *noncev = _mm256_add_epi32(*noncev, m256_const1_64(0x0000000400000000));
    hashes_done += 4.0;
#else
    // Increase nonce.
    edata0[19] += 2;
    edata1[19] += 2;
    gr_hash(hash, edata0, edata1, thr_id);
    hashes_done += 2.0;
#endif
    if (rotation == 0) {
      pthread_mutex_lock(&stats_lock);
      bench_hashes += hashes_done;
      pthread_mutex_unlock(&stats_lock);
      sync_bench();
      sync_bench();
      break;
    }
  }
}

static bool save_config() {
  char *filename = "tune_config";
  FILE *fd;
  fd = fopen(filename, "w");
  if (fd == NULL) {
    applog(LOG_ERR, "Could not save tune_config file");
    return false;
  }
  for (int i = 0; i < 20; i++) {
    fprintf(fd, "%d %d %d %d %d %d\n", cn_tune[i][0], cn_tune[i][1],
            cn_tune[i][2], cn_tune[i][3], cn_tune[i][4], cn_tune[i][5]);
  }
  fclose(fd);
  return true;
}

// Run tuning benchmarks and create tune_config in the end.
void tune(void *input, int thr_id) {
  for (int i = 0; i < 20; i++) {
    int best_hashrate = 0;
    if (thr_id == 0) {
      memset(cn_tune[i], 0, 6);
      applog(LOG_NOTICE, "Testing rotation: %d", i);
    }
    for (int config = 0; config < 8; config++) {
      memset(cn_config, 0, 6);
      cn_config[cn_map[cn[i][0]]] = (config & 1) >> 0;
      cn_config[cn_map[cn[i][1]]] = (config & 2) >> 1;
      cn_config[cn_map[cn[i][2]]] = (config & 4) >> 2;
      sync_conf();
      tune_config(input, thr_id, i);
      sync_conf();
      if (thr_id == 0) {
        if (best_hashrate < bench_hashrate) {
          cn_tune[i][cn_map[cn[i][0]]] = (config & 1) >> 0;
          cn_tune[i][cn_map[cn[i][1]]] = (config & 2) >> 1;
          cn_tune[i][cn_map[cn[i][2]]] = (config & 4) >> 2;

          best_hashrate = bench_hashrate;
        }
        bench_hashrate = 0;
        bench_time = 0;
        bench_hashes = 0;
      }
      // Right now config
      sync_conf();
    }
    if (thr_id == 0) {
      applog(LOG_NOTICE, "Best config for rotation %d: %d %d %d %d %d %d", i,
             cn_tune[i][0], cn_tune[i][1], cn_tune[i][2], cn_tune[i][3],
             cn_tune[i][4], cn_tune[i][5]);
    }
  }
  if (thr_id == 0) {
    for (int i = 0; i < 20; i++) {
      applog(LOG_NOTICE,
             "Best config for rotation %d (%d %d %d): %d %d %d %d %d %d", i,
             cn[i][0], cn[i][1], cn[i][2], cn_tune[i][0], cn_tune[i][1],
             cn_tune[i][2], cn_tune[i][3], cn_tune[i][4], cn_tune[i][5]);
    }
    opt_tune = false;
    opt_tuned = true;
    save_config();
  }
  sync_conf();
}

void benchmark(void *input, int thr_id, long sleep_time) {
  for (int i = 0; i < 160; i++) {
    ((uint8_t *)input)[i] = i;
  }

  srand(thr_id);
  pthread_t pthr;
  if (thr_id == 0) {
    pthread_create(&pthr, NULL, &statistic_thread,
                   sleep_time ? &sleep_time : NULL);
  }

  uint32_t n = 10000 * thr_id;
  uint32_t edata0[20] __attribute__((aligned(64)));
  mm128_bswap32_80(edata0, input);
  edata0[19] = n;
#if defined(GR_4WAY)
  uint32_t hash[8 * 4] __attribute__((aligned(64)));
  uint32_t vdata[20 * 4] __attribute__((aligned(64)));
  __m256i *noncev = (__m256i *)vdata + 9; // aligned
  mm256_bswap32_intrlv80_4x64(vdata, input);
  *noncev = mm256_intrlv_blend_32(
      _mm256_set_epi32(n + 3, 0, n + 2, 0, n + 1, 0, n, 0), *noncev);
#else
  uint32_t hash[8 * 2] __attribute__((aligned(64)));
  uint32_t edata1[20] __attribute__((aligned(64)));
  mm128_bswap32_80(edata1, input);
  edata1[19] = n + 1;
#endif
  uint8_t local_rotation = 255;
  double hashes_done = 0.0;

  sync_bench(); // Sync before benchmark starts.
  while (true) {
    if (likely(local_rotation != rotation)) {
      pthread_mutex_lock(&stats_lock);
#if defined(GR_4WAY)
      bench_hashes += hashes_done + 2.0;
#else
      bench_hashes += hashes_done + 1.0;
#endif
      pthread_mutex_unlock(&stats_lock);
      hashes_done = 0.0;
      // Change first part of the hash to get different core rotation.
      for (int i = 1; i < 5 + 1; ++i) {
        edata0[i] = rand();
      }
      // Use new rotation.
      gr_getAlgoString((const uint8_t *)(&edata0[1]), gr_hash_order);
      gr_hash_order[5] = cn[rotation][0] + 15;
      gr_hash_order[11] = cn[rotation][1] + 15;
      gr_hash_order[17] = cn[rotation][2] + 15;

      if (opt_tuned) {
        select_tuned_config(thr_id);
      }

      // Purge memory for test.
      AllocateNeededMemory();

      sync_bench(); // Rotation change sync.
      if (rotation == 0 && local_rotation != 255) {
        sync_bench(); // Rotation change sync.
        if (likely(stop_benchmark)) {
          return;
        }
      }
      local_rotation = rotation;
    } else {
#if defined(GR_4WAY)
      hashes_done += 4.0;
#else
      hashes_done += 2.0;
#endif
    }
#if defined(GR_4WAY)
    // Make sure nonces are increased for each hash. Same hashes will result
    // in better data locality on CN algos leading to better/innaccurate
    // results.
    gr_4way_hash(hash, vdata, thr_id);
    *noncev = _mm256_add_epi32(*noncev, m256_const1_64(0x0000000400000000));
#else
    // Increase nonce.
    edata0[19] += 2;
    edata1[19] += 2;
    gr_hash(hash, edata0, edata1, thr_id);
#endif
  }
}

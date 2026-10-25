/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define BUILD_TIER2   /* Running as Tier 2 recovery bootloader */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
CRC_HandleTypeDef hcrc;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_CRC_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#include "merkle_verify.h"
#include "uart_proto.h"
#include "flash_writer.h"
#include "otp_counter.h"
#include "../../Lib/sha256_sw.h"   /* Ablation benchmark only — not used in OTA path */
#include <string.h>
#include <stdio.h>

/* Default: this binary is the Tier 2 OTA bootloader.
   Override with -DBUILD_TIER1 or -DBUILD_TIER3 in compiler flags.
   This ensures the bootloader always works even if CubeIDE regenerates
   the makefile and drops the -DBUILD_TIER2 flag. */
#if !defined(BUILD_TIER1) && !defined(BUILD_TIER2) && !defined(BUILD_TIER3) && !defined(LOOPBACK_TEST)
#define BUILD_TIER2
#endif

#if defined(BUILD_TIER1)
// --- TIER 1 ---
#define TIER2_BASE      0x08004000UL
#define TIER2_SIZE      (16U * 1024U)
#define TIER2_SIG_OFFSET (TIER2_SIZE - 64U)
static const uint8_t OEM_PUBKEY[64] = { 0 };
static int ecdsa_verify_stub(const uint8_t *pubkey, const uint8_t *hash32, const uint8_t *sig64) { return 1; }
static void sha256_tier2(uint8_t *digest32) { memset(digest32, 0, 32); }
static void jump_to(uint32_t base) {
    uint32_t sp = *(volatile uint32_t*)(base);
    uint32_t pc = *(volatile uint32_t*)(base + 4);
    SCB->VTOR = base;
    __set_MSP(sp);
    ((void(*)(void))pc)();
}

#elif defined(BUILD_TIER2) || defined(LOOPBACK_TEST)
// --- TIER 2 ---
#define TIER3_BASE       0x08008000UL
#define TIER3_MAX_BLOCKS 120U          /* 120 x 4KB = 480KB (Sector 2 through 7 entirely) */


static uint8_t  rx_buf[BLOCK_SIZE + 2 + MAX_PROOF_DEPTH * HASH_LEN + 16];
static uint16_t failed[TIER3_MAX_BLOCKS];

static uint16_t read_num_blocks(void) { return *(volatile uint16_t*)META_NBLOCKS_ADDR; }
static uint32_t read_version(void) { return *(volatile uint32_t*)META_VERSION_ADDR; }
static hash_t* stored_leaves(void) { return (hash_t*)META_LEAVES_ADDR; }
static hash_t* stored_root(void) { return (hash_t*)META_ROOT_ADDR; }

static bool receive_ota(void) {
    uint8_t type; uint16_t len;
    
    flash_writer_session_reset();

    printf("[T2] Waiting for OTA_START...\r\n");
    while (1) {
        proto_send(MSG_READY, NULL, 0);
        if (proto_recv(&type, rx_buf, sizeof(rx_buf), &len, 1000)) {
            if (type == MSG_OTA_START) break;
        }
    }

    uint32_t version    = *(uint32_t*)(rx_buf + 0);
    uint16_t num_blocks = *(uint16_t*)(rx_buf + 4);
    hash_t root;
    memcpy(root.h, rx_buf + 6, HASH_LEN);
    
    if (num_blocks > TIER3_MAX_BLOCKS) {
        proto_nack();
        return false;
    }
    
    if (!flash_init_metadata(version, num_blocks, &root)) {
        proto_nack();
        return false;
    }
    
    proto_ack();

    static hash_t leaf_store[TIER3_MAX_BLOCKS];
    while (1) {
        if (!proto_recv(&type, rx_buf, sizeof(rx_buf), &len, 100000)) return false;
        if (type == MSG_OTA_START) NVIC_SystemReset(); /* Server aborted and restarted */
        if (type == MSG_OTA_END) break;
        if (type != MSG_BLOCK)  { proto_nack(); continue; }

        uint16_t bid = *(uint16_t*)(rx_buf + 0);
        uint8_t *data = rx_buf + 2;
        hash_t *proof = (hash_t*)(rx_buf + 2 + BLOCK_SIZE);
        if (bid >= num_blocks) { proto_nack(); continue; }

        hash_t leaf; merkle_leaf(&leaf, data, bid, version);
        uint8_t depth = 0; while ((1<<depth) < num_blocks) depth++;
        if (!merkle_verify(&leaf, bid, proof, depth, &root)) { proto_nack(); continue; }

        uint32_t addr = TIER3_BASE + (uint32_t)bid * BLOCK_SIZE;
        if (!flash_write_block(addr, data, BLOCK_SIZE)) { proto_nack(); continue; }
        if (!flash_update_leaf(bid, &leaf)) { proto_nack(); continue; }

        memcpy(leaf_store[bid].h, leaf.h, HASH_LEN);
        proto_ack();
    }

    flash_mark_complete();
    proto_send(MSG_OTA_COMPLETE, NULL, 0);
    return true;
}

static bool do_recovery(void) {
    /* Tell flash_writer to skip erasing sectors, because this is a 
       recovery and the sectors were already erased during OTA_START.
       Erasing them now would wipe intact blocks! */
    flash_writer_skip_erases();

    /* First check: valid metadata must start with the magic word.
       If flash was never written by this bootloader (e.g. freshly flashed
       MCU, stale code bytes from old -O0 build), magic won't match and
       we safely fall into a fresh OTA without crashing. */
    uint32_t magic = *(volatile uint32_t*)META_MAGIC_ADDR;
    if (magic != META_MAGIC_VALUE) return receive_ota();

    uint16_t num_blocks = read_num_blocks();
    uint32_t version = read_version();
    if (num_blocks == 0xFFFF || num_blocks == 0) return receive_ota();

    uint32_t status = *(volatile uint32_t*)META_STATUS_ADDR;
    uint16_t nfail = 0;
    
    if (status == META_STATUS_COMPLETE) {
        nfail = merkle_scan(TIER3_BASE, num_blocks, version, stored_leaves(), failed);
        if (nfail == 0) return true;
        /* Flash corrupted after complete, fall through to block req */
    } else {
        /* IN_PROGRESS: calculate missing blocks directly from leaves without scanning */
        const hash_t *meta_leaves = stored_leaves();
        for (int i = 0; i < num_blocks; i++) {
            bool empty = true;
            for (int j=0; j<32; j++) { if (meta_leaves[i].h[j] != 0xFF) empty = false; }
            if (empty) {
                failed[nfail++] = i;
            }
        }
        if (nfail == 0) {
            flash_mark_complete();
            return true;
        }
        /* If ALL blocks are missing, no progress was ever made.
           Treat this exactly like a fresh start so a re-run of the server
           with a different binary (different root) works correctly. */
        if (nfail == num_blocks) {
            return receive_ota();
        }
    }

    /* Prepare and send the block request */
    static uint8_t req_buf[2 + TIER3_MAX_BLOCKS * 2];
    
    req_buf[0] = nfail & 0xFF; 
    req_buf[1] = (nfail >> 8) & 0xFF;
    for (int i = 0; i < nfail; i++) {
        req_buf[2 + i*2] = failed[i] & 0xFF;
        req_buf[2 + i*2+1] = (failed[i] >> 8) & 0xFF;
    }
    
    printf("[T2] Requesting %d blocks...\r\n", nfail);
    uint8_t type; uint16_t len;
    while (1) {
        proto_send(MSG_BLOCK_REQ, req_buf, 2 + nfail * 2);
        if (proto_recv(&type, rx_buf, sizeof(rx_buf), &len, 3000)) {
            if (type == MSG_OTA_START) return receive_ota(); /* New firmware from server */
            if (type == MSG_BLOCK) break;
        }
    }

    /* We successfully received the FIRST block out of nfail. Process it! */
    uint16_t bid = *(uint16_t*)(rx_buf + 0);
    uint8_t *data = rx_buf + 2;
    hash_t *proof = (hash_t*)(rx_buf + 2 + BLOCK_SIZE);
    hash_t *root = stored_root();
    uint8_t depth = 0; while ((1<<depth) < num_blocks) depth++;

    hash_t leaf; merkle_leaf(&leaf, data, bid, version);
    if (merkle_verify(&leaf, bid, proof, depth, root)) {
        uint32_t addr = TIER3_BASE + (uint32_t)bid * BLOCK_SIZE;
        if (flash_write_block(addr, data, BLOCK_SIZE)) {
            flash_update_leaf(bid, &leaf);
            proto_ack();
        } else { proto_nack(); }
    } else { proto_nack(); }
    
    /* Process the REMAINING (nfail - 1) blocks normally without blasting MSG_BLOCK_REQ */
    for (int r = 1; r < nfail; r++) {
        if (!proto_recv(&type, rx_buf, sizeof(rx_buf), &len, 100000)) return false;
        if (type == MSG_OTA_START) NVIC_SystemReset(); /* Server aborted */
        if (type != MSG_BLOCK) return false;

        bid = *(uint16_t*)(rx_buf + 0);
        data = rx_buf + 2;
        proof = (hash_t*)(rx_buf + 2 + BLOCK_SIZE);
        
        merkle_leaf(&leaf, data, bid, version);
        if (!merkle_verify(&leaf, bid, proof, depth, root)) {
            proto_nack(); r--; continue;
        }
        uint32_t addr = TIER3_BASE + (uint32_t)bid * BLOCK_SIZE;
        if (!flash_write_block(addr, data, BLOCK_SIZE)) {
            proto_nack(); r--; continue;
        }
        if (!flash_update_leaf(bid, &leaf)) {
            proto_nack(); r--; continue;
        }
        proto_ack();
    }

    /* Wait for MSG_OTA_END from server before completing */
    while (1) {
        if (proto_recv(&type, rx_buf, sizeof(rx_buf), &len, 5000)) {
            if (type == MSG_OTA_END) {
                break;
            }
        }
    }

    flash_mark_complete();
    proto_send(MSG_OTA_COMPLETE, NULL, 0);
    return true;
}

/* Call this after OTA_COMPLETE, before jumping to Tier 3 */
static void flash_verify_server_mode(uint16_t num_blocks)
{
    printf("[T2] Entering flash verify mode...\r\n");
    uint8_t type;
    uint16_t len;
    static uint8_t rx[8];

    for (;;) {
        if (!proto_recv(&type, rx, sizeof(rx), &len, 100000)) break;
        if (type != MSG_READ_REQ) break;

        uint16_t bid = rx[0] | ((uint16_t)rx[1] << 8);
        if (bid >= num_blocks) break;

        uint8_t *block_ptr = (uint8_t*)(TIER3_BASE + bid * BLOCK_SIZE);
        proto_send(MSG_READ_RESP, block_ptr, BLOCK_SIZE);
    }
    printf("[T2] Verify mode done.\r\n");
}

static void jump_tier3(void) {
    uint32_t sp = *(volatile uint32_t*)TIER3_BASE;
    uint32_t pc = *(volatile uint32_t*)(TIER3_BASE + 4);
    SCB->VTOR = TIER3_BASE;
    __set_MSP(sp);
    ((void(*)(void))pc)();
}

#elif defined(BUILD_TIER3)
// --- TIER 3 ---
#endif

#ifndef APP_VERSION
#define APP_VERSION 1
#endif

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */
  /* USER CODE END 1 */

  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_CRC_Init();

  /* USER CODE BEGIN 2 */
#if defined(BUILD_TIER1)
    printf("\r\n[T1] Tier 1 ROM Bootloader\r\n");
    uint8_t digest[32];
    sha256_tier2(digest);
    uint8_t *sig = (uint8_t*)(TIER2_BASE + TIER2_SIG_OFFSET);
    if (ecdsa_verify_stub(OEM_PUBKEY, digest, sig)) {
        printf("[T1] Tier 2 OK — jumping\r\n");
        jump_to(TIER2_BASE);
    } else {
        printf("[T1] Tier 2 INVALID — halting\r\n");
        while (1) { HAL_Delay(300); }
    }
#elif defined(BUILD_TIER2) || defined(LOOPBACK_TEST)
    printf("\r\n[T2] Tier 2 Bootloader — Merkle Recovery\r\n");
    
    uint32_t stored_ver = otp_read_version();
    uint32_t fw_ver = read_version();
    if (fw_ver != 0xFFFFFFFF && fw_ver < stored_ver) {
        printf("[T2] ROLLBACK BLOCKED\r\n");
        /* Temporarily disabled for benchmark measurements */
        // while (1) { HAL_Delay(500); HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin); }
    }
    
    if (!do_recovery()) {
        printf("[T2] Recovery failed. Halting.\r\n");
        while (1) { HAL_Delay(200); HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin); }
    }
    if (fw_ver != 0xFFFFFFFF) otp_update_version(fw_ver);

    /* --- Run Table 2 Measurements --- */
    printf("\r\n--- TABLE 2 BENCHMARKS ---\r\n");
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    uint32_t t_start, t_end;
    uint32_t ms_int, ms_frac;
    
    // 1. Leaf Hash Time (4KB block)
    static uint8_t dummy_block[BLOCK_SIZE];
    hash_t dummy_leaf;
    DWT->CYCCNT = 0; t_start = DWT->CYCCNT;
    merkle_leaf(&dummy_leaf, dummy_block, 0, 1);
    t_end = DWT->CYCCNT;
    ms_int = (t_end - t_start) / 180000;
    ms_frac = ((t_end - t_start) % 180000) * 1000 / 180000;
    printf("1. Leaf hash (4KB, SHAKE-256) : %lu.%03lu ms\r\n", ms_int, ms_frac);

    // 2. Proof Verify (depth=7 for 128 blocks)
    hash_t dummy_proof[7];
    hash_t dummy_root;
    DWT->CYCCNT = 0; t_start = DWT->CYCCNT;
    merkle_verify(&dummy_leaf, 0, dummy_proof, 7, &dummy_root);
    t_end = DWT->CYCCNT;
    ms_int = (t_end - t_start) / 180000;
    ms_frac = ((t_end - t_start) % 180000) * 1000 / 180000;
    printf("2. Proof verify (depth=7)     : %lu.%03lu ms\r\n", ms_int, ms_frac);

    // 3. Proof Verify (depth=8 for 256 blocks)
    hash_t dummy_proof8[8];
    DWT->CYCCNT = 0; t_start = DWT->CYCCNT;
    merkle_verify(&dummy_leaf, 0, dummy_proof8, 8, &dummy_root);
    t_end = DWT->CYCCNT;
    ms_int = (t_end - t_start) / 180000;
    ms_frac = ((t_end - t_start) % 180000) * 1000 / 180000;
    printf("3. Proof verify (depth=8)     : %lu.%03lu ms\r\n", ms_int, ms_frac);

    // 4. Merkle Scan (Extrapolate to 128 blocks from one block scan)
    DWT->CYCCNT = 0; t_start = DWT->CYCCNT;
    for(int i=0; i<128; i++) {
        merkle_leaf(&dummy_leaf, dummy_block, 0, 1);
    }
    t_end = DWT->CYCCNT;
    ms_int = (t_end - t_start) / 180000;
    ms_frac = ((t_end - t_start) % 180000) * 1000 / 180000;
    printf("4. Full scan time (128 blocks): %lu.%03lu ms\r\n", ms_int, ms_frac);
    printf("----------------------------\r\n\r\n");

    /* --- Run SHA-256 Ablation Benchmarks (TABLE 2B) --- */
    printf("--- SHA-256 ABLATION BENCHMARKS ---\r\n");

    /* Helper inline: SHA-256 of (block || block_id(2B LE) || version(4B LE))
       Mirrors exactly the merkle_leaf() domain separation used by SHAKE-256. */
    static uint8_t sha_tmp[BLOCK_SIZE + 6];  /* same layout as merkle_leaf's tmp[] */
    uint8_t sha_digest[32];
    sha256_ctx_t sha_ctx;

    /* 1. SHA-256 Leaf Hash: hash 4KB block + 6 bytes domain suffix */
    memset(sha_tmp, 0, sizeof(sha_tmp));  /* dummy_block contents (all-zero same as SHAKE test) */
    sha_tmp[BLOCK_SIZE + 0] = 0;          /* block_id = 0, LE */
    sha_tmp[BLOCK_SIZE + 1] = 0;
    sha_tmp[BLOCK_SIZE + 2] = 1;          /* version = 1, LE */
    sha_tmp[BLOCK_SIZE + 3] = 0;
    sha_tmp[BLOCK_SIZE + 4] = 0;
    sha_tmp[BLOCK_SIZE + 5] = 0;

    DWT->CYCCNT = 0; t_start = DWT->CYCCNT;
    sha256_init(&sha_ctx);
    sha256_update(&sha_ctx, sha_tmp, BLOCK_SIZE + 6);
    sha256_final(&sha_ctx, sha_digest);
    t_end = DWT->CYCCNT;
    ms_int  = (t_end - t_start) / 180000;
    ms_frac = ((t_end - t_start) % 180000) * 1000 / 180000;
    printf("A. Leaf hash (4KB, SHA-256)   : %lu.%03lu ms\r\n", ms_int, ms_frac);

    /* 2. SHA-256 Proof Verify (depth=7): 7 calls of sha256 over 64-byte node pairs.
       Mirror merkle_verify(): at each level we hash (left||right) = 64 bytes. */
    uint8_t node_pair[64];  /* two 32-byte hashes concatenated */
    memcpy(node_pair,      sha_digest, 32);  /* current = leaf digest */
    memset(node_pair + 32, 0xAB, 32);        /* dummy sibling proof hash */

    DWT->CYCCNT = 0; t_start = DWT->CYCCNT;
    for (int lvl = 0; lvl < 7; lvl++) {
        /* At each level: hash(current || sibling) or hash(sibling || current).
           Both are 64-byte inputs — same cost either way. */
        sha256_oneshot(node_pair, 64, sha_digest);   /* result is new node */
        /* Feed result back as left input for next level */
        memcpy(node_pair, sha_digest, 32);
    }
    t_end = DWT->CYCCNT;
    ms_int  = (t_end - t_start) / 180000;
    ms_frac = ((t_end - t_start) % 180000) * 1000 / 180000;
    printf("B. Proof verify (depth=7)     : %lu.%03lu ms\r\n", ms_int, ms_frac);

    /* 3. SHA-256 Full Scan: 128 leaf hashes (same loop as SHAKE-256 test above) */
    DWT->CYCCNT = 0; t_start = DWT->CYCCNT;
    for (int i = 0; i < 128; i++) {
        sha256_init(&sha_ctx);
        sha256_update(&sha_ctx, sha_tmp, BLOCK_SIZE + 6);
        sha256_final(&sha_ctx, sha_digest);
    }
    t_end = DWT->CYCCNT;
    ms_int  = (t_end - t_start) / 180000;
    ms_frac = ((t_end - t_start) % 180000) * 1000 / 180000;
    printf("C. Full scan (128 x SHA-256)  : %lu.%03lu ms\r\n", ms_int, ms_frac);
    printf("-----------------------------------\r\n\r\n");

    /* Firmware is healthy. Give the server a 10-second window to start 
       a NEW OTA update before we boot into the application. */
    printf("[T2] Firmware healthy. Advertising for 10 seconds...\r\n");
    uint8_t ota_type; uint16_t ota_len;
    for (int i = 0; i < 20; i++) { /* 20 * 500ms = 10s */
        proto_send(MSG_READY, NULL, 0);
        if (proto_recv(&ota_type, rx_buf, sizeof(rx_buf), &ota_len, 500)) {
            if (ota_type == MSG_OTA_START) {
                printf("[T2] Server initiated new OTA update!\r\n");
                /* Invalidate the metadata so receive_ota() treats it as fresh. */
                HAL_FLASH_Unlock();
                HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, META_MAGIC_ADDR, 0x00000000);
                HAL_FLASH_Lock();
                /* We consumed OTA_START, so let's NACK it and let the server retry.
                   When receive_ota() runs, the server's retry will trigger it. */
                proto_nack();
                receive_ota();
                break;
            }
        }
    }

    flash_verify_server_mode(read_num_blocks());

    printf("[T2] Jumping to Tier 3...\r\n");
    HAL_Delay(100);
    jump_tier3();
#else
    printf("\r\n[T3] Application booted — OTA successful!\r\n");
    printf("[T3] Version: %lu\r\n", (unsigned long)APP_VERSION);
#endif
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
#if !defined(BUILD_TIER1) && !defined(BUILD_TIER2) && !defined(LOOPBACK_TEST)
    HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
    HAL_Delay(500);
    printf("[T3] Running...\r\n");
#endif
    /* USER CODE END WHILE */
  }
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief CRC Initialization Function
  * @param None
  * @retval None
  */
static void MX_CRC_Init(void)
{

  /* USER CODE BEGIN CRC_Init 0 */

  /* USER CODE END CRC_Init 0 */

  /* USER CODE BEGIN CRC_Init 1 */

  /* USER CODE END CRC_Init 1 */
  hcrc.Instance = CRC;
  if (HAL_CRC_Init(&hcrc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CRC_Init 2 */

  /* USER CODE END CRC_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : LD2_Pin */
  GPIO_InitStruct.Pin = LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

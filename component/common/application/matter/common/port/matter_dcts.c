/************************** 
* Matter DCT Related 
**************************/
#include "platform_opts.h"
#include "platform/platform_stdlib.h"

#ifdef __cplusplus
 extern "C" {
#endif

#include "stddef.h"
#include "string.h"
#include "stdbool.h"
#include "dct.h"
#include "chip_porting.h"

#if CONFIG_ENABLE_DCT_ENCRYPTION
#include "mbedtls/aes.h"
#endif
/*
   module size is 4k, we set max module number as 12;
   if backup enabled, the total module number is 12 + 1*12 = 24, the size is 96k;
   if wear leveling enabled, the total module number is 12 + 2*12 + 3*12 = 36, the size is 288k"
*/
#define DCT_BEGIN_ADDR_MATTER   DCT_BEGIN_ADDR    /*!< DCT begin address of flash, ex: 0x100000 = 1M */
#define MODULE_NUM              13                /*!< max number of module */
#define VARIABLE_NAME_SIZE      32                /*!< max size of the variable name */
#define VARIABLE_VALUE_SIZE     64 + 4            /*!< max size of the variable value, +4 is required, else the max variable size we can store is 60 */ 
                                                  /*!< max number of variable in module = floor (4024 / (32 + 64)) = 41 */

#define DCT_BEGIN_ADDR_MATTER2  DCT_BEGIN_ADDR2
#define MODULE_NUM2             10
#define VARIABLE_NAME_SIZE2     32
#define VARIABLE_VALUE_SIZE2    400 + 4           /* +4 is required, else the max variable size we can store is 396 */
                                                  /*!< max number of variable in module = floor (4024 / (32 + 400)) = 9 */

#define ENABLE_BACKUP           0
#define ENABLE_WEAR_LEVELING    0

#if CONFIG_ENABLE_DCT_ENCRYPTION
#if defined(MBEDTLS_CIPHER_MODE_CTR)
mbedtls_aes_context aes;

// key length 32 bytes for 256 bit encrypting, it can be 16 or 24 bytes for 128 and 192 bits encrypting mode
unsigned char key[] = {0xff, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xff, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

#define DCT_REGION_1 0
#define DCT_REGION_2 1

int32_t dct_encrypt(unsigned char *input_to_encrypt, int input_len, unsigned char *encrypt_output)
{
    size_t nc_off = 0;

    unsigned char nonce_counter[16] = {0};
    unsigned char stream_block[16] = {0};

    return mbedtls_aes_crypt_ctr(&aes, input_len, &nc_off, nonce_counter, stream_block, input_to_encrypt, encrypt_output);
}

int32_t dct_decrypt(unsigned char *input_to_decrypt, int input_len, unsigned char *decrypt_output)
{
    size_t nc_off1 = 0;
    unsigned char nonce_counter1[16] = {0};
    unsigned char stream_block1[16] = {0};

    return mbedtls_aes_crypt_ctr(&aes, input_len, &nc_off1, nonce_counter1, stream_block1, input_to_decrypt, decrypt_output);
}

int32_t dct_set_encrypted_variable(dct_handle_t *dct_handle, char *variable_name, char *variable_value, uint16_t variable_value_length, uint8_t region)
{
    int32_t ret;
    char encrypted_data[VARIABLE_VALUE_SIZE2] = {0};

    // encrypt the variable value
    ret = dct_encrypt(variable_value, variable_value_length, encrypted_data);
    if (ret != 0)
    {
       return DCT_ERROR;
    }

    // store in dct
    if (region == DCT_REGION_1)
        ret = dct_set_variable_new(dct_handle, variable_name, encrypted_data, variable_value_length);
    else if (region == DCT_REGION_2)
        ret = dct_set_variable_new2(dct_handle, variable_name, encrypted_data, variable_value_length);

    return ret;
}

int32_t dct_get_encrypted_variable(dct_handle_t *dct_handle, char *variable_name, char *buffer, uint16_t *buffer_size, uint8_t region)
{
    int32_t ret;
    uint8_t encrypted_data[404] = {0};

    // get the encrypted value from dct
    if (region == DCT_REGION_1)
        ret = dct_get_variable_new(dct_handle, variable_name, encrypted_data, buffer_size);
    else if (region == DCT_REGION_2)
        ret = dct_get_variable_new2(dct_handle, variable_name, encrypted_data, buffer_size);

    if (ret != DCT_SUCCESS)
        return ret;

    // decrypt the encrypted value
    ret = dct_decrypt(encrypted_data, *buffer_size, buffer);
    if (ret != 0)
    {
        return DCT_ERROR;
    }
    
    return ret;
}

#else
#error "MBEDTLS_CIPHER_MODE_CTR must be enabled to perform DCT flash encryption" 
#endif // MBEDTLS_CIPHER_MODE_CTR
#endif

#if defined(DCT_UPDATE_ENABLE) && DCT_UPDATE_ENABLE
#include "utility.h"
#include <flash_api.h>
#include <device_lock.h>

/**
 * Please change the DCT_BEGIN_ADDR_OLD and DCT_BEGIN_ADDR2_OLD before using dct_update
 */

#define MODULE_NAME_OFFSET      8
#define MODULE_INFO_SIZE        32
#define MODULE_NUM_OFFSET       44
#define DCT_BACKUP_OFFSET       52

void dct1_update_settings(void)
{
    flash_t flash;
    int count, value, i;
    uint8_t *buffer;
    uint32_t buffer_size = 0x1000;
    uint8_t new_module_name[15];
    int cur_module_num = MODULE_NUM;
    int prev_module_num = 0;
    int write_flash = 0;

    buffer = rtw_malloc(buffer_size);
    if (!buffer)
    {
        printf("dct_check_swap: malloc error\n");
        goto exit;
    }

    // 1. Loop through DCT1
    for (i = 0; i < MODULE_NUM; i ++)
    {
        // 2. Read from flash
        device_mutex_lock(RT_DEV_LOCK_FLASH);
        flash_stream_read(&flash, DCT_BEGIN_ADDR_OLD+(i*buffer_size), buffer_size, buffer);
        device_mutex_unlock(RT_DEV_LOCK_FLASH);

        if(buffer[0] != 0xFF)
        {
            // 3. Ensure that flash data belongs to DCT1
            if (strncmp(buffer, "DCT1", 4) == 0)
            {
                // 4. For v1.0 Module Name changed. If module name starts with "chip" must change.
                if (strncmp(buffer+MODULE_NAME_OFFSET, "chip", 4) == 0)
                {
                    value = i + 1;
                    snprintf(new_module_name, 15, "matter_kvs1_%d", value);
                    for (count = 0; count < sizeof(new_module_name); count++)
                    {
                        buffer[MODULE_NAME_OFFSET+count] = new_module_name[count];
                    }
                    memset(buffer+MODULE_NAME_OFFSET+count, 0, MODULE_INFO_SIZE-MODULE_NAME_OFFSET-count);
                    write_flash = 1;
                }

                // 5. If Backup value differs
                if(buffer[DCT_BACKUP_OFFSET] != ENABLE_BACKUP)
                {
                    memset(buffer+DCT_BACKUP_OFFSET, ENABLE_BACKUP, 1);
                    write_flash = 1;
                }

                // 6. If module number has changed, set new module number
                if (prev_module_num != cur_module_num)
                {
                    memset(buffer+MODULE_NUM_OFFSET, cur_module_num, 1);
                    write_flash = 1;
                }

                // 7. If DCT address has changed, set write_flash.
                if (DCT_BEGIN_ADDR_OLD != DCT_BEGIN_ADDR_MATTER)
                {
                    write_flash = 1;
                }

                if (write_flash)
                {
                    device_mutex_lock(RT_DEV_LOCK_FLASH);
                     //8. Erase flash data from old DCT address (DCT_BEGIN_ADDR_OLD)
                    flash_erase_sector(&flash, DCT_BEGIN_ADDR_OLD+(i*buffer_size));
                    // 9. Erase flash data in new DCT address (DCT_BEGIN_ADDR_MATTER)
                    flash_erase_sector(&flash, DCT_BEGIN_ADDR_MATTER+(i*buffer_size));
                    // 10. Write data to new DCT address (DCT_BEGIN_ADDR_MATTER)
                    flash_stream_write(&flash, DCT_BEGIN_ADDR_MATTER+(i*buffer_size), buffer_size, buffer);
                    device_mutex_unlock(RT_DEV_LOCK_FLASH);

                    write_flash = 0;
                }
            }
        }
    }

exit:
    if(buffer)
    {
        rtw_free(buffer);
    }

    return;
}

void dct2_update_settings(void)
{
    flash_t flash;
    int count, value, i;
    uint8_t *buffer;
    uint32_t buffer_size = 0x1000;
    uint8_t new_module_name[15];
    int cur_module_num = MODULE_NUM2;
    int prev_module_num = 0;
    int write_flash = 0;

    buffer = rtw_malloc(buffer_size);
    if (!buffer)
    {
        printf("dct_check_swap: malloc error\n");
        goto exit;
    }

    // 1. Loop through DCT1
    for (i = 0; i < MODULE_NUM2; i ++)
    {
        // 2. Read from flash
        device_mutex_lock(RT_DEV_LOCK_FLASH);
        flash_stream_read(&flash, DCT_BEGIN_ADDR2_OLD+(i*buffer_size), buffer_size, buffer);
        device_mutex_unlock(RT_DEV_LOCK_FLASH);

        if(buffer[0] != 0xFF)
        {
            // 3. Ensure that flash data belongs to DCT1
            if (strncmp(buffer, "DCT2", 4) == 0)
            {
                // 4. For v1.0 Module Name changed. If module name starts with "chip" must change.
                if (strncmp(buffer+MODULE_NAME_OFFSET, "chip", 4) == 0)
                {
                    value = i + 1;
                    snprintf(new_module_name, 15, "matter_kvs2_%d", value);
                    for (count = 0; count < sizeof(new_module_name); count++)
                    {
                        buffer[MODULE_NAME_OFFSET+count] = new_module_name[count];
                    }
                    memset(buffer+MODULE_NAME_OFFSET+count, 0, MODULE_INFO_SIZE-MODULE_NAME_OFFSET-count);
                    write_flash = 1;
                }

                // 5. If Backup value changed, set backup value
                if(buffer[DCT_BACKUP_OFFSET] != ENABLE_BACKUP)
                {
                    memset(buffer+DCT_BACKUP_OFFSET, ENABLE_BACKUP, 1);
                    write_flash = 1;
                }

                // 6. If module number has changed, set new module number
                if (prev_module_num != cur_module_num)
                {
                    memset(buffer+MODULE_NUM_OFFSET, cur_module_num, 1);
                    write_flash = 1;
                }

                //7. If DCT address has changed, set write_flash.
                if (DCT_BEGIN_ADDR2_OLD != DCT_BEGIN_ADDR_MATTER2)
                {
                    write_flash = 1;
                }

                if (write_flash)
                {
                    device_mutex_lock(RT_DEV_LOCK_FLASH);
                     //8. Erase flash data from old DCT address (DCT_BEGIN_ADDR2_OLD)
                    flash_erase_sector(&flash, DCT_BEGIN_ADDR2_OLD+(i*buffer_size));
                    // 9. Erase flash data in new DCT address (DCT_BEGIN_ADDR_MATTER2)
                    flash_erase_sector(&flash, DCT_BEGIN_ADDR_MATTER2+(i*buffer_size));
                    // 10. Write data to new DCT address (DCT_BEGIN_ADDR_MATTER2)
                    flash_stream_write(&flash, DCT_BEGIN_ADDR_MATTER2+(i*buffer_size), buffer_size, buffer);
                    device_mutex_unlock(RT_DEV_LOCK_FLASH);

                    write_flash = 0;
                }
            }
        }
    }

exit:
    if(buffer)
    {
        rtw_free(buffer);
    }

    return;
}
#endif

s32 initPref(void)
{
    s32 ret;

#if defined(DCT_UPDATE_ENABLE) && DCT_UPDATE_ENABLE
    printf("\nDCT change happening!!\n");
    dct1_update_settings();
    dct2_update_settings();
#endif

    ret = dct_init(DCT_BEGIN_ADDR_MATTER, MODULE_NUM, VARIABLE_NAME_SIZE, VARIABLE_VALUE_SIZE, ENABLE_BACKUP, ENABLE_WEAR_LEVELING);
    if (ret != DCT_SUCCESS)
        printf("dct_init failed with error: %d\n", ret);
    else
        printf("dct_init success\n");

    ret = dct_init2(DCT_BEGIN_ADDR_MATTER2, MODULE_NUM2, VARIABLE_NAME_SIZE2, VARIABLE_VALUE_SIZE2, ENABLE_BACKUP, ENABLE_WEAR_LEVELING);
    if (ret != DCT_SUCCESS)
        printf("dct_init2 failed with error: %d\n", ret);
    else
        printf("dct_init2 success\n");

#if CONFIG_ENABLE_DCT_ENCRYPTION
    // Initialize mbedtls aes context and set encryption key
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, key, 256) != 0)
    {
        return DCT_ERROR;
    }
#endif

    return ret;
}

s32 deinitPref(void)
{
    s32 ret;
    ret = dct_format(DCT_BEGIN_ADDR_MATTER, MODULE_NUM, VARIABLE_NAME_SIZE, VARIABLE_VALUE_SIZE, ENABLE_BACKUP, ENABLE_WEAR_LEVELING);
    if (ret != DCT_SUCCESS)
        printf("dct_format failed with error: %d\n", ret);
    else
        printf("dct_format success\n");

    ret = dct_format2(DCT_BEGIN_ADDR_MATTER2, MODULE_NUM2, VARIABLE_NAME_SIZE2, VARIABLE_VALUE_SIZE2, ENABLE_BACKUP, ENABLE_WEAR_LEVELING);
    if (ret != DCT_SUCCESS)
        printf("dct_format2 failed with error: %d\n", ret);
    else
        printf("dct_format2 success\n");

#if CONFIG_ENABLE_DCT_ENCRYPTION
    // free aes context
    mbedtls_aes_free(&aes);
#endif

    return ret;
}

s32 registerPref()
{
    s32 ret;
    char ns[15];

    for (size_t i=0; i<MODULE_NUM; i++)
    {
        snprintf(ns, 15, "matter_kvs1_%d", i+1); 
        ret = dct_register_module(ns);
        if (ret != DCT_SUCCESS)
            goto exit;
        else
            printf("dct_register_module %s success\n", ns);
    }

exit:
    if (ret != DCT_SUCCESS)
        printf("DCT1 modules registration failed");
    return ret;
}

s32 registerPref2()
{
    s32 ret;
    char ns[15];

    for (size_t i=0; i<MODULE_NUM2; i++)
    {
        snprintf(ns, 15, "matter_kvs2_%d", i+1); 
        ret = dct_register_module2(ns);
        if (ret != DCT_SUCCESS)
            goto exit;
        else
            printf("dct_register_module2 %s success\n", ns);
    }

exit:
    if (ret != DCT_SUCCESS)
        printf("DCT2 modules registration failed");
    return ret;
}

s32 clearPref()
{
    s32 ret;
    char ns[15];

    for (size_t i=0; i<MODULE_NUM; i++)
    {
        snprintf(ns, 15, "matter_kvs1_%d", i+1); 
        ret = dct_unregister_module(ns);
        if (ret != DCT_SUCCESS)
            goto exit;
        else
            printf("dct_unregister_module %s success\n", ns);
    }

exit:
    if (ret != DCT_SUCCESS)
        printf("DCT1 modules unregistration failed");
    return ret;
}

s32 clearPref2()
{
    s32 ret;
    char ns[15];

    for (size_t i=0; i<MODULE_NUM2; i++)
    {
        snprintf(ns, 15, "matter_kvs2_%d", i+1); 
        ret = dct_unregister_module2(ns);
        if (ret != DCT_SUCCESS)
            goto exit;
        else
            printf("dct_unregister_module2 %s success\n", ns);
    }

exit:
    if (ret != DCT_SUCCESS)
        printf("DCT2 modules unregistration failed");
    return ret;
}

s32 deleteKey(const char *domain, const char *key)
{
    dct_handle_t handle;
    s32 ret;
    char ns[15];

    // Loop over DCT1 modules
    for (size_t i=0; i<MODULE_NUM; i++)
    {
        snprintf(ns, 15, "matter_kvs1_%d", i+1); 
        ret = dct_open_module(&handle, ns);
        if (ret != DCT_SUCCESS)
        {
            printf("%s : dct_open_module(%s) failed with error: %d\n" ,__FUNCTION__, ns, ret);
            goto exit;
        }
        ret = dct_delete_variable(&handle, key);
        dct_close_module(&handle);
        if (ret == DCT_SUCCESS) // return success once deleted
            return ret;
    }

    // Loop over DCT2 modules
    for (size_t i=0; i<MODULE_NUM2; i++)
    {
        snprintf(ns, 15, "matter_kvs2_%d", i+1); 
        ret = dct_open_module2(&handle, ns);
        if (ret != DCT_SUCCESS)
        {
            printf("%s : dct_open_module2(%s) failed with error: %d\n" ,__FUNCTION__, ns, ret);
            goto exit;
        }
        ret = dct_delete_variable2(&handle, key);
        dct_close_module2(&handle);
        if (ret == DCT_SUCCESS) // return success once deleted
            return ret;
    }

exit:
    return ret;
}

bool checkExist(const char *domain, const char *key)
{
    dct_handle_t handle;
    s32 ret;
    uint16_t len = 0;
    u8 *str = malloc(sizeof(u8) * VARIABLE_VALUE_SIZE2); // use the bigger buffer size
    char ns[15];

    // Loop over DCT1 modules
    for (size_t i=0; i<MODULE_NUM; i++)
    {
        snprintf(ns, 15, "matter_kvs1_%d", i+1); 
        ret = dct_open_module(&handle, ns);
        if (ret != DCT_SUCCESS)
        {
            printf("%s : dct_open_module(%s) failed with error: %d\n" ,__FUNCTION__, ns, ret);
            goto exit;
        }

        len = sizeof(u32);
        ret = dct_get_variable_new(&handle, key, (char *)str, &len);
        if(ret == DCT_SUCCESS)
        {
            printf("checkExist key=%s found.\n", key);
            dct_close_module(&handle);
            goto exit;
        }

        len = sizeof(u64);
        ret = dct_get_variable_new(&handle, key, (char *)str, &len);
        if(ret == DCT_SUCCESS)
        {
            printf("checkExist key=%s found.\n", key);
            dct_close_module(&handle);
            goto exit;
        }

        dct_close_module(&handle);
    }

    // Loop over DCT2 modules
    for (size_t i=0; i<MODULE_NUM2; i++)
    {
        snprintf(ns, 15, "matter_kvs2_%d", i+1); 
        ret = dct_open_module2(&handle, ns);
        if (ret != DCT_SUCCESS)
        {
            printf("%s : dct_open_module2(%s) failed with error : %d\n" ,__FUNCTION__, ns, ret);
            goto exit;
        }

        len = VARIABLE_VALUE_SIZE2;
        ret = dct_get_variable_new2(&handle, key, str, &len);
        if(ret == DCT_SUCCESS)
        {
            printf("checkExist key=%s found.\n", key);
            dct_close_module2(&handle);
            goto exit;
        }

        dct_close_module2(&handle);
    }

exit:
    free(str);
    return (ret == DCT_SUCCESS) ? true : false;
}

s32 setPref_new(const char *domain, const char *key, u8 *value, size_t byteCount)
{
    dct_handle_t handle;
    s32 ret;
    char ns[15];

    if (byteCount <= 64)
    {
        // Loop over DCT1 modules
        for (size_t i=0; i<MODULE_NUM; i++)
        {
            snprintf(ns, 15, "matter_kvs1_%d", i+1); 
            ret = dct_open_module(&handle, ns);
            if (ret != DCT_SUCCESS)
            {
                printf("%s : dct_open_module(%s) failed with error: %d\n" ,__FUNCTION__, ns, ret);
                goto exit;
            }

            if (dct_remain_variable(&handle) > 0)
            {
#if CONFIG_ENABLE_DCT_ENCRYPTION
                ret = dct_set_encrypted_variable(&handle, key, value, byteCount, DCT_REGION_1);
#else
                ret = dct_set_variable_new(&handle, key, (char *)value, (uint16_t)byteCount);
#endif
                if (ret != DCT_SUCCESS)
                {
                    printf("%s : dct_set_variable(%s) failed with error: %d\n" ,__FUNCTION__, key, ret);
                    dct_close_module(&handle);
                    goto exit;
                }
                dct_close_module(&handle);
                break;
            }
            dct_close_module(&handle);
        }
    }
    else
    {
        // Loop over DCT2 modules
        for (size_t i=0; i<MODULE_NUM2; i++)
        {
            snprintf(ns, 15, "matter_kvs2_%d", i+1); 
            ret = dct_open_module2(&handle, ns);
            if (ret != DCT_SUCCESS)
            {
                printf("%s : dct_open_module2(%s) failed with error: %d\n" ,__FUNCTION__, ns, ret);
                goto exit;
            }

            if (dct_remain_variable2(&handle) > 0)
            {
#if CONFIG_ENABLE_DCT_ENCRYPTION
                ret = dct_set_encrypted_variable(&handle, key, value, byteCount, DCT_REGION_2);
#else
                ret = dct_set_variable_new2(&handle, key, (char *)value, (uint16_t)byteCount);
#endif
                if (ret != DCT_SUCCESS)
                {
                    printf("%s : dct_set_variable2(%s) failed with error: %d\n" ,__FUNCTION__, key, ret);
                    dct_close_module2(&handle);
                    goto exit;
                }
                dct_close_module2(&handle);
                break;
            }
            dct_close_module2(&handle);
        }
    }

exit:
    return ret;
}

s32 getPref_bool_new(const char *domain, const char *key, u8 *val)
{
    dct_handle_t handle;
    s32 ret;
    uint16_t len = sizeof(u8);
    char ns[15];

    // Loop over DCT1 modules
    for (size_t i=0; i<MODULE_NUM; i++)
    {
        snprintf(ns, 15, "matter_kvs1_%d", i+1); 
        ret = dct_open_module(&handle, ns);
        if (ret != DCT_SUCCESS)
        {
            printf("%s : dct_open_module(%s) failed with error: %d\n" ,__FUNCTION__, ns, ret);
            goto exit;
        }
#if CONFIG_ENABLE_DCT_ENCRYPTION
        ret = dct_get_encrypted_variable(&handle, key, (char *)val, &len, DCT_REGION_1);
#else
        ret = dct_get_variable_new(&handle, key, (char *)val, &len);
#endif
        dct_close_module(&handle);
        if (ret == DCT_SUCCESS)
        {
            return ret;
        }
    }

    // Loop over DCT2 modules
    for (size_t i=0; i<MODULE_NUM2; i++)
    {
        snprintf(ns, 15, "matter_kvs2_%d", i+1); 
        ret = dct_open_module2(&handle, ns);
        if (ret != DCT_SUCCESS)
        {
            printf("%s : dct_open_module2(%s) failed with error: %d\n" ,__FUNCTION__, ns, ret);
            goto exit;
        }
#if CONFIG_ENABLE_DCT_ENCRYPTION
        ret = dct_get_encrypted_variable(&handle, key, (char *)val, &len, DCT_REGION_2);
#else
        ret = dct_get_variable_new2(&handle, key, (char *)val, &len);
#endif
        dct_close_module2(&handle);
        if (ret == DCT_SUCCESS)
        {
            return ret;
        }
    }

exit:
    return ret;
}

s32 getPref_u32_new(const char *domain, const char *key, u32 *val)
{
    dct_handle_t handle;
    s32 ret;
    uint16_t len = sizeof(u32);
    char ns[15];

    // Loop over DCT1 modules
    for (size_t i=0; i<MODULE_NUM; i++)
    {
        snprintf(ns, 15, "matter_kvs1_%d", i+1); 
        ret = dct_open_module(&handle, ns);
        if (ret != DCT_SUCCESS)
        {
            printf("%s : dct_open_module(%s) failed with error: %d\n" ,__FUNCTION__, ns, ret);
            goto exit;
        }
#if CONFIG_ENABLE_DCT_ENCRYPTION
        ret = dct_get_encrypted_variable(&handle, key, (char *)val, &len, DCT_REGION_1);
#else
        ret = dct_get_variable_new(&handle, key, (char *)val, &len);
#endif
        dct_close_module(&handle);
        if (ret == DCT_SUCCESS)
        {
            return ret;
        }
    }

    // Loop over DCT2 modules
    for (size_t i=0; i<MODULE_NUM2; i++)
    {
        snprintf(ns, 15, "matter_kvs2_%d", i+1); 
        ret = dct_open_module2(&handle, ns);
        if (ret != DCT_SUCCESS)
        {
            printf("%s : dct_open_module2(%s) failed with error: %d\n" ,__FUNCTION__, ns, ret);
            goto exit;
        }
#if CONFIG_ENABLE_DCT_ENCRYPTION
        ret = dct_get_encrypted_variable(&handle, key, (char *)val, &len, DCT_REGION_2);
#else
        ret = dct_get_variable_new2(&handle, key, (char *)val, &len);
#endif
        dct_close_module2(&handle);
        if (ret == DCT_SUCCESS)
        {
            return ret;
        }
    }

exit:
    return ret;
}

s32 getPref_u64_new(const char *domain, const char *key, u64 *val)
{
    dct_handle_t handle;
    s32 ret;
    uint16_t len = sizeof(u64);
    char ns[15];

    // Loop over DCT1 modules
    for (size_t i=0; i<MODULE_NUM; i++)
    {
        snprintf(ns, 15, "matter_kvs1_%d", i+1); 
        ret = dct_open_module(&handle, ns);
        if (ret != DCT_SUCCESS)
        {
            printf("%s : dct_open_module(%s) failed with error: %d\n" ,__FUNCTION__, ns, ret);
            goto exit;
        }
#if CONFIG_ENABLE_DCT_ENCRYPTION
        ret = dct_get_encrypted_variable(&handle, key, (char *)val, &len, DCT_REGION_1);
#else
        ret = dct_get_variable_new(&handle, key, (char *)val, &len);
#endif
        dct_close_module(&handle);
        if (ret == DCT_SUCCESS)
        {
            return ret;
        }
    }

    // Loop over DCT2 modules
    for (size_t i=0; i<MODULE_NUM2; i++)
    {
        snprintf(ns, 15, "matter_kvs2_%d", i+1); 
        ret = dct_open_module2(&handle, ns);
        if (ret != DCT_SUCCESS)
        {
            printf("%s : dct_open_module2(%s) failed with error: %d\n" ,__FUNCTION__, ns, ret);
            goto exit;
        }
#if CONFIG_ENABLE_DCT_ENCRYPTION
        ret = dct_get_encrypted_variable(&handle, key, (char *)val, &len, DCT_REGION_2);
#else
        ret = dct_get_variable_new2(&handle, key, (char *)val, &len);
#endif
        dct_close_module2(&handle);
        if (ret == DCT_SUCCESS)
        {
            return ret;
        }
    }

exit:
    return ret;
}

s32 getPref_str_new(const char *domain, const char *key, char * buf, size_t bufSize, size_t *outLen)
{
    dct_handle_t handle;
    s32 ret;
    char ns[15];

    // Loop over DCT1 modules
    for (size_t i=0; i<MODULE_NUM; i++)
    {
        snprintf(ns, 15, "matter_kvs1_%d", i+1); 
        ret = dct_open_module(&handle, ns);
        if (ret != DCT_SUCCESS)
        {
            printf("%s : dct_open_module(%s) failed with error: %d\n" ,__FUNCTION__, ns, ret);
            goto exit;
        }
#if CONFIG_ENABLE_DCT_ENCRYPTION
        ret = dct_get_encrypted_variable(&handle, key, buf, &bufSize, DCT_REGION_1);
#else
        ret = dct_get_variable_new(&handle, key, buf, &bufSize);
#endif
        dct_close_module(&handle);
        if (ret == DCT_SUCCESS)
        {
            *outLen = bufSize;
            return ret;
        }
    }

    // Loop over DCT2 modules
    for (size_t i=0; i<MODULE_NUM2; i++)
    {
        snprintf(ns, 15, "matter_kvs2_%d", i+1); 
        ret = dct_open_module2(&handle, ns);
        if (ret != DCT_SUCCESS)
        {
            printf("%s : dct_open_module2(%s) failed with error: %d\n" ,__FUNCTION__, ns, ret);
            goto exit;
        }
#if CONFIG_ENABLE_DCT_ENCRYPTION
        ret = dct_get_encrypted_variable(&handle, key, buf, &bufSize, DCT_REGION_2);
#else
        ret = dct_get_variable_new2(&handle, key, buf, &bufSize);
#endif
        dct_close_module2(&handle);
        if (ret == DCT_SUCCESS)
        {
            *outLen = bufSize;
            return ret;
        }
    }

exit:
    return ret;
}

s32 getPref_bin_new(const char *domain, const char *key, u8 * buf, size_t bufSize, size_t *outLen)
{
    dct_handle_t handle;
    s32 ret;
    char ns[15];

    // Loop over DCT1 modules
    for (size_t i=0; i<MODULE_NUM; i++)
    {
        snprintf(ns, 15, "matter_kvs1_%d", i+1); 
        ret = dct_open_module(&handle, ns);
        if (ret != DCT_SUCCESS)
        {
            printf("%s : dct_open_module(%s) failed with error: %d\n" ,__FUNCTION__, ns, ret);
            goto exit;
        }
#if CONFIG_ENABLE_DCT_ENCRYPTION
        ret = dct_get_encrypted_variable(&handle, key, (char *)buf, &bufSize, DCT_REGION_1);
#else
        ret = dct_get_variable_new(&handle, key, (char *)buf, &bufSize);
#endif
        dct_close_module(&handle);
        if (ret == DCT_SUCCESS)
        {
            *outLen = bufSize;
            return ret;
        }
    }

    // Loop over DCT2 modules
    for (size_t i=0; i<MODULE_NUM2; i++)
    {
        snprintf(ns, 15, "matter_kvs2_%d", i+1); 
        ret = dct_open_module2(&handle, ns);
        if (ret != DCT_SUCCESS)
        {
            printf("%s : dct_open_module2(%s) failed with error: %d\n" ,__FUNCTION__, ns, ret);
            goto exit;
        }
#if CONFIG_ENABLE_DCT_ENCRYPTION
        ret = dct_get_encrypted_variable(&handle, key, (char *)buf, &bufSize, DCT_REGION_2);
#else
        ret = dct_get_variable_new2(&handle, key, (char *)buf, &bufSize);
#endif
        dct_close_module2(&handle);
        if (ret == DCT_SUCCESS)
        {
            *outLen = bufSize;
            return ret;
        }
    }

exit:
    return ret;
}

#ifdef __cplusplus
}
#endif

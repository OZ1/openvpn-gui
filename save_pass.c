#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <windows.h>
#include <wincrypt.h>

#include "main.h"
#include "registry.h"
#include "save_pass.h"
#include "passphrase.h"

#define KEY_PASS_DATA     L"key-data"
#define AUTH_PASS_DATA    L"auth-data"
#define TOTP_PASS_DATA    L"totp-data"
#define ENTROPY_DATA      L"entropy"
#define AUTH_USER_DATA    L"username"
#define ENTROPY_LEN 16

static DWORD
crypt_protect(BYTE *data, int szdata, char *entropy, BYTE **out)
{
    DATA_BLOB data_out;
    DATA_BLOB data_in;
    DATA_BLOB e;

    data_in.pbData = data;
    data_in.cbData = szdata;
    e.pbData = (BYTE*) entropy;
    e.cbData = entropy? strlen(entropy) : 0;

    if(CryptProtectData(&data_in, NULL, &e, NULL, NULL, 0, &data_out))
    {
        *out = data_out.pbData;
        return data_out.cbData;
    }
    PrintDebug(L"CryptProtectData failed (error = %lu)", GetLastError());
    return 0;
}

static DWORD
crypt_unprotect(BYTE *data, int szdata, char *entropy, BYTE **out)
{
    DATA_BLOB data_in;
    DATA_BLOB data_out = {0,0};
    DATA_BLOB e;

    data_in.pbData = data;
    data_in.cbData = szdata;
    e.pbData = (BYTE *) entropy;
    e.cbData = entropy? strlen(entropy) : 0;

    if(CryptUnprotectData(&data_in, NULL, &e, NULL, NULL, 0, &data_out))
    {
        *out = data_out.pbData;
        return data_out.cbData;
    }
    else
    {
        PrintDebug(L"CryptUnprotectData: decryption failed");
        LocalFree (data_out.pbData);
        return 0;
    }
}

/*
 * If not found in registry and generate is true, a new nul terminated
 * random string is generated and saved in registry.
 * Else a zero-length string is returned and registry is not updated.
 */
static void
get_entropy(const WCHAR *config_name, char *e, int sz, BOOL generate)
{
    int len;

    len = GetConfigRegistryValue(config_name, ENTROPY_DATA, (BYTE *) e, sz);
    if (len > 0)
    {
        e[len-1] = '\0';
        PrintDebug(L"Got entropy from registry: %S (len = %d)", e, len);
        return;
    }
    else if (generate && GetRandomPassword(e, sz))
    {
        e[sz-1] = '\0';
        PrintDebug(L"Created new entropy string : %S", e);
        if (SetConfigRegistryValueBinary(config_name, ENTROPY_DATA, (BYTE *)e, sz))
            return;
    }
    if (generate)
        PrintDebug(L"Failed to generate or save new entropy string -- using null string");
    *e = '\0';
    return;
}
/*
 * Given a nul terminated string password, encrypt it and save in
 * a config specific registry key with specified name.
 * Returns 1 on success.
 */
static int
save_encrypted_data(const WCHAR *config_name, const WCHAR *name, const void *password, DWORD len)
{
    BYTE *out;
    char entropy[ENTROPY_LEN+1];

    get_entropy(config_name, entropy, sizeof(entropy), true);
    len = crypt_protect(password, len, entropy, &out);
    if(len > 0)
    {
        SetConfigRegistryValueBinary(config_name, name, out, len);
        LocalFree(out);
        return 1;
    }
    else
        return 0;
}
/*
 * Given a nul terminated string password, encrypt it and save in
 * a config specific registry key with specified name.
 * Returns 1 on success.
 */
static int
save_encrypted(const WCHAR *config_name, const WCHAR *password, const WCHAR *name)
{
    return save_encrypted_data(config_name, name, password, (wcslen(password) + 1) * sizeof(WCHAR));
}

/*
 * Encrypt the nul terminated string password and store it in the
 * registry with key name KEY_PASS_DATA. Returns 1 on success.
 */
int
SaveKeyPass(const WCHAR *config_name, const WCHAR *password)
{
    return save_encrypted(config_name, password, KEY_PASS_DATA);
}

/*
 * Encrypt the nul terminated string password and store it in the
 * registry with key name AUTH_PASS_DATA. Returns 1 on success.
 */
int
SaveAuthPass(const WCHAR *config_name, const WCHAR *password)
{
    return save_encrypted(config_name, password, AUTH_PASS_DATA);
}

/*
 * Encrypt the nul terminated string password and store it in the
 * registry with key name TOTP_PASS_DATA. Returns 1 on success.
 */
int
SaveTotpPass(const WCHAR *config_name, const char *password)
{
    return save_encrypted_data(config_name, TOTP_PASS_DATA, password, strlen(password));
}

/*
 * Returns length on success, 0 on failure. password should have space
 * for up to capacity wide chars incluing nul termination
 */
static int
recall_encrypted_data(const WCHAR *config_name, const WCHAR *name, void **out)
{
    BYTE in[2048];
    DWORD len = GetConfigRegistryValue(config_name, name, in, sizeof(in));
    if(len == 0)
        return 0;

    char entropy[ENTROPY_LEN+1];
    get_entropy(config_name, entropy, sizeof(entropy), false);
    return crypt_unprotect(in, len, entropy, (BYTE**)out);
}
/*
 * Returns 1 on success, 0 on failure. password should have space
 * for up to capacity wide chars incluing nul termination
 */
static int
recall_encrypted(const WCHAR *config_name, WCHAR *password, DWORD capacity, const WCHAR *name)
{
    BYTE *out;
    DWORD len;
    int retval = 0;

    memset(password, 0, capacity);

    len = recall_encrypted_data(config_name, name, &out);
    if(len == 0)
        return 0;

    if (len <= capacity)
    {
        memcpy(password, out, len);
        password[capacity/sizeof(WCHAR)-1] = L'\0'; /* in case the data was corrupted */
        retval = len;
    }
    else
        PrintDebug(L"recall_encrypted: saved '%s' too long (len = %d bytes)", name, len);

    SecureZeroMemory(out, len);
    LocalFree(out);

    return retval;
}
static int
recall_encrypted_1(const WCHAR *config_name, char *password, DWORD capacity, const WCHAR *name)
{
    BYTE *out;
    DWORD len;
    int retval = 0;

    memset(password, 0, capacity);

    len = recall_encrypted_data(config_name, name, &out);
    if(len == 0)
        return 0;

    if (len <= capacity)
    {
        memcpy(password, out, len);
        password[capacity-1] = L'\0'; /* in case the data was corrupted */
        retval = len;
    }
    else
        PrintDebug(L"recall_encrypted: saved '%s' too long (len = %d bytes)", name, len);

    SecureZeroMemory(out, len);
    LocalFree(out);

    return retval;
}


/*
 * Reccall saved private key password. The buffer password should be
 * have space for up to KEY_PASS_LEN WCHARs including nul.
 * Returns 1 on success, 0 on failure.
 */
int
RecallKeyPass(const WCHAR *config_name, WCHAR *password, DWORD size)
{
    return recall_encrypted(config_name, password, size, KEY_PASS_DATA);
}

/*
 * Reccall saved auth password. The buffer password should be
 * have space for up to USER_PASS_LEN WCHARs including nul.
 * Returns 1 on success, 0 on failure.
 */
int
RecallAuthPass(const WCHAR *config_name, WCHAR *password, DWORD size)
{
    return recall_encrypted(config_name, password, size, AUTH_PASS_DATA);
}

/*
 * Reccall saved auth password. The buffer password should be
 * have space for up to USER_PASS_LEN WCHARs including nul.
 * Returns 1 on success, 0 on failure.
 */
int
RecallTotpPass(const WCHAR *config_name, char *password, DWORD size)
{
    return recall_encrypted_1(config_name, password, size, TOTP_PASS_DATA);
}

int
SaveUsername(const WCHAR *config_name, const WCHAR *username)
{
    DWORD len = (wcslen(username) + 1) * sizeof(*username);
    SetConfigRegistryValueBinary(config_name, AUTH_USER_DATA, (const void*)username, len);
    return 1;
}
/*
 * The buffer username should be have space for up to USER_PASS_LEN
 * WCHARs including nul.
 */
int
RecallUsername(const WCHAR *config_name, WCHAR *username, DWORD capacity)
{
    DWORD len = GetConfigRegistryValue(config_name, AUTH_USER_DATA, (BYTE *)username, capacity);
    if (len == 0)
        return 0;
    username[capacity-1] = L'\0';
    return len;
}

void
DeleteSavedKeyPass(const WCHAR *config_name)
{
    DeleteConfigRegistryValue(config_name, KEY_PASS_DATA);
}

void
DeleteSavedAuthPass(const WCHAR *config_name)
{
    DeleteConfigRegistryValue(config_name, AUTH_PASS_DATA);
}

void
DeleteSavedTotpPass(const WCHAR *config_name)
{
    DeleteConfigRegistryValue(config_name, TOTP_PASS_DATA);
}

/* delete saved config-specific auth password and private key passphrase */
void
DeleteSavedPasswords(const WCHAR *config_name)
{
    DeleteConfigRegistryValue(config_name, KEY_PASS_DATA);
    DeleteConfigRegistryValue(config_name, AUTH_PASS_DATA);
    DeleteConfigRegistryValue(config_name, TOTP_PASS_DATA);
    DeleteConfigRegistryValue(config_name, ENTROPY_DATA);
}

/* check if auth password is saved */
BOOL
IsAuthPassSaved(const WCHAR *config_name)
{
    DWORD len = GetConfigRegistryValue(config_name, AUTH_PASS_DATA, NULL, 0);
    PrintDebug(L"checking auth-data in registry returned len = %d", len);
    return (len > 0);
}

/* check if TOTP password is saved */
BOOL
IsTotpPassSaved(const WCHAR *config_name)
{
    DWORD len = GetConfigRegistryValue(config_name, TOTP_PASS_DATA, NULL, 0);
    PrintDebug(L"checking totp-data in registry returned len = %d", len);
    return (len > 0);
}

/* check if key password is saved */
BOOL
IsKeyPassSaved(const WCHAR *config_name)
{
    DWORD len = GetConfigRegistryValue(config_name, KEY_PASS_DATA, NULL, 0);
    PrintDebug(L"checking key-data in registry returned len = %d", len);
    return (len > 0);
}

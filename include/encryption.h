#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>

#include <cstdint>
#include <vector>

class CryptoChannel {
public:
  static const int NONCE_LEN = 16;
  static const int IV_LEN    = 16;
  static const int MAC_LEN   = 32;

  CryptoChannel();
  ~CryptoChannel();

  // Handshake + key derivation
  bool init_server(SOCKET s); // sends nonce_s, reads nonce_c
  bool init_client(SOCKET s); // reads nonce_s, sends nonce_c

  // Encrypted framed send/recv (maintains seq numbers internally)
  bool send_msg(SOCKET s, const std::vector<uint8_t>& plaintext);
  bool recv_msg(SOCKET s, std::vector<uint8_t>& plaintext_out);

private:
  // Internal state
  bool ready_;
  uint64_t seq_out_;
  uint64_t seq_in_;

  uint8_t k_enc_[32];
  uint8_t k_mac_[32];

  // AES expanded key (AES-256 => 240 bytes)
  uint8_t aes_round_key_[240];

  // Helpers (all implemented in encryption.cpp)
  static bool read_exact(SOCKET s, void* buf, int n);
  static bool write_exact(SOCKET s, const void* buf, int n);

  static void write_u32_be(uint8_t out[4], uint32_t v);
  static uint32_t read_u32_be(const uint8_t in[4]);
  static void write_u64_be(uint8_t out[8], uint64_t v);
  static uint64_t read_u64_be(const uint8_t in[8]);

  static bool rng_bytes(uint8_t* out, DWORD n);

  static void derive_keys(const uint8_t nonce_s[NONCE_LEN],
                          const uint8_t nonce_c[NONCE_LEN],
                          uint8_t k_enc[32],
                          uint8_t k_mac[32]);

  static void compute_frame_mac(const uint8_t k_mac[32],
                                uint64_t seq,
                                const uint8_t iv[IV_LEN],
                                const uint8_t* ct, uint32_t ct_len,
                                uint8_t out[MAC_LEN]);

  // AES-256-CTR
  void aes_key_expand_256(const uint8_t key[32], uint8_t roundKey[240]);
  void aes256_encrypt_block(const uint8_t in[16], uint8_t out[16], const uint8_t roundKey[240]);
  void aes256_ctr_xor(uint8_t iv[16], uint8_t* data, size_t len, const uint8_t roundKey[240]);

  // SHA256 / HMAC
  static void hmac_sha256(const uint8_t* key, size_t keylen,
                          const uint8_t* msg, size_t msglen,
                          uint8_t out[32]);
};
#define WIN32_LEAN_AND_MEAN
#include "encryption.h"

#include <wincrypt.h>
#include <cstring>
#include <string>

// Link: -ladvapi32
#pragma comment(lib, "advapi32.lib")

// ===================== PSK (shared secret) =====================
// Change this to your own random 32 bytes.
static const uint8_t PSK[32] = {
  0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
  0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,
  0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80,
  0x90,0xa0,0xb0,0xc0,0xd0,0xe0,0xf0,0x01
};

// ===================== socket IO =====================
bool CryptoChannel::read_exact(SOCKET s, void* buf, int n) {
  char* p = (char*)buf;
  int got = 0;
  while (got < n) {
    int r = recv(s, p + got, n - got, 0);
    if (r <= 0) return false;
    got += r;
  }
  return true;
}
bool CryptoChannel::write_exact(SOCKET s, const void* buf, int n) {
  const char* p = (const char*)buf;
  int sent = 0;
  while (sent < n) {
    int w = send(s, p + sent, n - sent, 0);
    if (w <= 0) return false;
    sent += w;
  }
  return true;
}

void CryptoChannel::write_u32_be(uint8_t out[4], uint32_t v) {
  out[0] = (uint8_t)((v >> 24) & 0xFF);
  out[1] = (uint8_t)((v >> 16) & 0xFF);
  out[2] = (uint8_t)((v >>  8) & 0xFF);
  out[3] = (uint8_t)( v        & 0xFF);
}
uint32_t CryptoChannel::read_u32_be(const uint8_t in[4]) {
  return (uint32_t(in[0]) << 24) | (uint32_t(in[1]) << 16) | (uint32_t(in[2]) << 8) | uint32_t(in[3]);
}
void CryptoChannel::write_u64_be(uint8_t out[8], uint64_t v) {
  for (int i = 7; i >= 0; --i) { out[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}
uint64_t CryptoChannel::read_u64_be(const uint8_t in[8]) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | uint64_t(in[i]);
  return v;
}

// ===================== RNG (CryptoAPI) =====================
bool CryptoChannel::rng_bytes(uint8_t* out, DWORD n) {
  HCRYPTPROV hProv = 0;
  if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return false;
  BOOL ok = CryptGenRandom(hProv, n, out);
  CryptReleaseContext(hProv, 0);
  return ok == TRUE;
}

// ===================== SHA256 + HMAC =====================
struct Sha256 {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t data[64];
  uint32_t datalen;

  static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
  static uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
  static uint32_t maj(uint32_t x, uint32_t y, uint32_t z){ return (x & y) ^ (x & z) ^ (y & z); }
  static uint32_t ep0(uint32_t x){ return rotr(x,2) ^ rotr(x,13) ^ rotr(x,22); }
  static uint32_t ep1(uint32_t x){ return rotr(x,6) ^ rotr(x,11) ^ rotr(x,25); }
  static uint32_t sig0(uint32_t x){ return rotr(x,7) ^ rotr(x,18) ^ (x >> 3); }
  static uint32_t sig1(uint32_t x){ return rotr(x,17)^ rotr(x,19) ^ (x >> 10); }

  static const uint32_t k[64];

  void init() {
    datalen = 0; bitlen = 0;
    state[0]=0x6a09e667; state[1]=0xbb67ae85; state[2]=0x3c6ef372; state[3]=0xa54ff53a;
    state[4]=0x510e527f; state[5]=0x9b05688c; state[6]=0x1f83d9ab; state[7]=0x5be0cd19;
  }

  void transform() {
    uint32_t m[64];
    for (int i=0;i<16;i++){
      m[i] = (uint32_t(data[i*4])<<24) | (uint32_t(data[i*4+1])<<16) | (uint32_t(data[i*4+2])<<8) | uint32_t(data[i*4+3]);
    }
    for (int i=16;i<64;i++) m[i] = sig1(m[i-2]) + m[i-7] + sig0(m[i-15]) + m[i-16];

    uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
    for (int i=0;i<64;i++){
      uint32_t t1 = h + ep1(e) + ch(e,f,g) + k[i] + m[i];
      uint32_t t2 = ep0(a) + maj(a,b,c);
      h=g; g=f; f=e; e=d + t1;
      d=c; c=b; b=a; a=t1 + t2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
  }

  void update(const uint8_t* in, size_t len) {
    for (size_t i=0;i<len;i++){
      data[datalen++] = in[i];
      if (datalen == 64) { transform(); bitlen += 512; datalen = 0; }
    }
  }

  void final(uint8_t out[32]) {
    uint32_t i = datalen;
    if (datalen < 56) { data[i++] = 0x80; while (i < 56) data[i++] = 0x00; }
    else { data[i++] = 0x80; while (i < 64) data[i++] = 0x00; transform(); std::memset(data, 0, 56); }

    bitlen += uint64_t(datalen) * 8;
    data[63]=(uint8_t)(bitlen); data[62]=(uint8_t)(bitlen>>8); data[61]=(uint8_t)(bitlen>>16); data[60]=(uint8_t)(bitlen>>24);
    data[59]=(uint8_t)(bitlen>>32); data[58]=(uint8_t)(bitlen>>40); data[57]=(uint8_t)(bitlen>>48); data[56]=(uint8_t)(bitlen>>56);
    transform();

    for (int j=0;j<8;j++){
      out[j*4+0] = (uint8_t)(state[j] >> 24);
      out[j*4+1] = (uint8_t)(state[j] >> 16);
      out[j*4+2] = (uint8_t)(state[j] >> 8);
      out[j*4+3] = (uint8_t)(state[j]);
    }
  }
};

const uint32_t Sha256::k[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

void CryptoChannel::hmac_sha256(const uint8_t* key, size_t keylen,
                                const uint8_t* msg, size_t msglen,
                                uint8_t out[32]) {
  uint8_t k0[64]; std::memset(k0, 0, 64);

  if (keylen > 64) {
    Sha256 s; s.init(); s.update(key, keylen); s.final(k0);
  } else {
    std::memcpy(k0, key, keylen);
  }

  uint8_t o_key[64], i_key[64];
  for (int i=0;i<64;i++){ o_key[i] = k0[i] ^ 0x5c; i_key[i] = k0[i] ^ 0x36; }

  uint8_t inner[32];
  Sha256 si; si.init();
  si.update(i_key, 64);
  si.update(msg, msglen);
  si.final(inner);

  Sha256 so; so.init();
  so.update(o_key, 64);
  so.update(inner, 32);
  so.final(out);
}

// ===================== AES-256 CTR =====================
static const uint8_t sbox_aes[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static uint8_t rcon(uint8_t i){
  uint8_t c=1; if(i==0) return 0;
  while(i!=1){ uint8_t b=c&0x80; c<<=1; if(b) c^=0x1B; i--; }
  return c;
}

static uint8_t xtime(uint8_t x){ return (uint8_t)((x<<1)^((x>>7)*0x1B)); }

static void sub_bytes(uint8_t* s){ for(int i=0;i<16;i++) s[i]=sbox_aes[s[i]]; }
static void shift_rows(uint8_t* s){
  uint8_t t;
  t=s[1]; s[1]=s[5]; s[5]=s[9]; s[9]=s[13]; s[13]=t;
  t=s[2]; s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
  t=s[3]; s[3]=s[15]; s[15]=s[11]; s[11]=s[7]; s[7]=t;
}
static void mix_columns(uint8_t* s){
  for(int c=0;c<4;c++){
    int i=c*4;
    uint8_t a0=s[i],a1=s[i+1],a2=s[i+2],a3=s[i+3];
    uint8_t t=a0^a1^a2^a3, u=a0;
    s[i]   ^= t ^ xtime((uint8_t)(a0^a1));
    s[i+1] ^= t ^ xtime((uint8_t)(a1^a2));
    s[i+2] ^= t ^ xtime((uint8_t)(a2^a3));
    s[i+3] ^= t ^ xtime((uint8_t)(a3^u));
  }
}

void CryptoChannel::aes_key_expand_256(const uint8_t key[32], uint8_t roundKey[240]) {
  std::memcpy(roundKey, key, 32);
  int bytes=32, rci=1; uint8_t tmp[4];

  auto sub_word = [](uint8_t* w){ for(int i=0;i<4;i++) w[i]=sbox_aes[w[i]]; };
  auto rot_word = [](uint8_t* w){ uint8_t t=w[0]; w[0]=w[1]; w[1]=w[2]; w[2]=w[3]; w[3]=t; };

  while(bytes<240){
    for(int i=0;i<4;i++) tmp[i]=roundKey[bytes-4+i];
    if(bytes%32==0){ rot_word(tmp); sub_word(tmp); tmp[0]^=rcon((uint8_t)rci++); }
    else if(bytes%32==16){ sub_word(tmp); }
    for(int i=0;i<4;i++){ roundKey[bytes]=roundKey[bytes-32]^tmp[i]; bytes++; }
  }
}

void CryptoChannel::aes256_encrypt_block(const uint8_t in[16], uint8_t out[16], const uint8_t roundKey[240]) {
  uint8_t s[16];
  std::memcpy(s, in, 16);

  auto add_round_key = [&](int r){
    const uint8_t* rk = roundKey + r*16;
    for(int i=0;i<16;i++) s[i] ^= rk[i];
  };

  add_round_key(0);
  for(int r=1;r<=13;r++){
    sub_bytes(s); shift_rows(s); mix_columns(s); add_round_key(r);
  }
  sub_bytes(s); shift_rows(s); add_round_key(14);

  std::memcpy(out, s, 16);
}

void CryptoChannel::aes256_ctr_xor(uint8_t iv[16], uint8_t* data, size_t len, const uint8_t roundKey[240]) {
  uint8_t stream[16];
  size_t off=0;
  while(off<len){
    aes256_encrypt_block(iv, stream, roundKey);
    size_t n = (len-off<16)?(len-off):16;
    for(size_t i=0;i<n;i++) data[off+i] ^= stream[i];

    // increment big-endian counter
    for(int i=15;i>=0;i--){ iv[i]++; if(iv[i]!=0) break; }
    off += n;
  }
}

// ===================== key derivation + MAC =====================
void CryptoChannel::derive_keys(const uint8_t nonce_s[NONCE_LEN],
                                const uint8_t nonce_c[NONCE_LEN],
                                uint8_t k_enc[32],
                                uint8_t k_mac[32]) {
  uint8_t msg_enc[3 + NONCE_LEN + NONCE_LEN];
  std::memcpy(msg_enc, "enc", 3);
  std::memcpy(msg_enc + 3, nonce_s, NONCE_LEN);
  std::memcpy(msg_enc + 3 + NONCE_LEN, nonce_c, NONCE_LEN);
  hmac_sha256(PSK, 32, msg_enc, sizeof(msg_enc), k_enc);

  uint8_t msg_mac[3 + NONCE_LEN + NONCE_LEN];
  std::memcpy(msg_mac, "mac", 3);
  std::memcpy(msg_mac + 3, nonce_s, NONCE_LEN);
  std::memcpy(msg_mac + 3 + NONCE_LEN, nonce_c, NONCE_LEN);
  hmac_sha256(PSK, 32, msg_mac, sizeof(msg_mac), k_mac);
}

void CryptoChannel::compute_frame_mac(const uint8_t k_mac[32],
                                      uint64_t seq,
                                      const uint8_t iv[IV_LEN],
                                      const uint8_t* ct, uint32_t ct_len,
                                      uint8_t out[MAC_LEN]) {
  std::vector<uint8_t> buf(8 + IV_LEN + ct_len);
  write_u64_be(buf.data(), seq);
  std::memcpy(buf.data() + 8, iv, IV_LEN);
  if (ct_len) std::memcpy(buf.data() + 8 + IV_LEN, ct, ct_len);
  hmac_sha256(k_mac, 32, buf.data(), buf.size(), out);
}

// ===================== class =====================
CryptoChannel::CryptoChannel() : ready_(false), seq_out_(0), seq_in_(0) {
  std::memset(k_enc_, 0, sizeof(k_enc_));
  std::memset(k_mac_, 0, sizeof(k_mac_));
  std::memset(aes_round_key_, 0, sizeof(aes_round_key_));
}
CryptoChannel::~CryptoChannel() {
  // wipe keys
  std::memset(k_enc_, 0, sizeof(k_enc_));
  std::memset(k_mac_, 0, sizeof(k_mac_));
  std::memset(aes_round_key_, 0, sizeof(aes_round_key_));
}

bool CryptoChannel::init_server(SOCKET s) {
  uint8_t nonce_s[NONCE_LEN], nonce_c[NONCE_LEN];
  if (!rng_bytes(nonce_s, NONCE_LEN)) return false;
  if (!write_exact(s, nonce_s, NONCE_LEN)) return false;
  if (!read_exact(s, nonce_c, NONCE_LEN)) return false;

  derive_keys(nonce_s, nonce_c, k_enc_, k_mac_);
  aes_key_expand_256(k_enc_, aes_round_key_);

  seq_out_ = 0;
  seq_in_  = 0;
  ready_ = true;
  return true;
}

bool CryptoChannel::init_client(SOCKET s) {
  uint8_t nonce_s[NONCE_LEN], nonce_c[NONCE_LEN];
  if (!read_exact(s, nonce_s, NONCE_LEN)) return false;
  if (!rng_bytes(nonce_c, NONCE_LEN)) return false;
  if (!write_exact(s, nonce_c, NONCE_LEN)) return false;

  derive_keys(nonce_s, nonce_c, k_enc_, k_mac_);
  aes_key_expand_256(k_enc_, aes_round_key_);

  seq_out_ = 0;
  seq_in_  = 0;
  ready_ = true;
  return true;
}

bool CryptoChannel::send_msg(SOCKET s, const std::vector<uint8_t>& plaintext) {
  if (!ready_) return false;

  uint8_t iv[IV_LEN];
  if (!rng_bytes(iv, IV_LEN)) return false;

  std::vector<uint8_t> ct = plaintext;
  uint8_t iv_work[16];
  std::memcpy(iv_work, iv, 16);
  if (!ct.empty()) aes256_ctr_xor(iv_work, ct.data(), ct.size(), aes_round_key_);

  uint8_t mac[MAC_LEN];
  compute_frame_mac(k_mac_, seq_out_, iv, ct.data(), (uint32_t)ct.size(), mac);

  uint32_t total_len = 8 + IV_LEN + (uint32_t)ct.size() + MAC_LEN;
  uint8_t lenb[4];
  write_u32_be(lenb, total_len);

  std::vector<uint8_t> frame(total_len);
  write_u64_be(frame.data(), seq_out_);
  std::memcpy(frame.data() + 8, iv, IV_LEN);
  if (!ct.empty()) std::memcpy(frame.data() + 8 + IV_LEN, ct.data(), ct.size());
  std::memcpy(frame.data() + 8 + IV_LEN + ct.size(), mac, MAC_LEN);

  if (!write_exact(s, lenb, 4)) return false;
  if (!write_exact(s, frame.data(), (int)frame.size())) return false;

  seq_out_++;
  return true;
}

bool CryptoChannel::recv_msg(SOCKET s, std::vector<uint8_t>& plaintext_out) {
  plaintext_out.clear();
  if (!ready_) return false;

  uint8_t lenb[4];
  if (!read_exact(s, lenb, 4)) return false;
  uint32_t total_len = read_u32_be(lenb);

  if (total_len < (8 + IV_LEN + MAC_LEN) || total_len > (1024 * 1024)) return false;

  std::vector<uint8_t> frame(total_len);
  if (!read_exact(s, frame.data(), (int)frame.size())) return false;

  const uint8_t* p = frame.data();
  uint64_t seq = read_u64_be(p);
  if (seq != seq_in_) return false; // strict ordering

  const uint8_t* iv  = p + 8;
  const uint8_t* ct  = p + 8 + IV_LEN;
  uint32_t ct_len = total_len - (8 + IV_LEN + MAC_LEN);
  const uint8_t* mac = p + 8 + IV_LEN + ct_len;

  uint8_t mac_calc[MAC_LEN];
  compute_frame_mac(k_mac_, seq, iv, ct, ct_len, mac_calc);
  if (std::memcmp(mac, mac_calc, MAC_LEN) != 0) return false;

  plaintext_out.assign(ct, ct + ct_len);
  uint8_t iv_work[16];
  std::memcpy(iv_work, iv, 16);
  if (!plaintext_out.empty()) aes256_ctr_xor(iv_work, plaintext_out.data(), plaintext_out.size(), aes_round_key_);

  seq_in_++;
  return true;
}
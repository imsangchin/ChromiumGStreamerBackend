// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/webcrypto/test/test_helpers.h"

#include <algorithm>

#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "components/test_runner/test_common.h"
#include "components/webcrypto/algorithm_dispatch.h"
#include "components/webcrypto/crypto_data.h"
#include "components/webcrypto/generate_key_result.h"
#include "components/webcrypto/jwk.h"
#include "components/webcrypto/status.h"
#include "components/webcrypto/webcrypto_util.h"
#include "third_party/WebKit/public/platform/WebCryptoAlgorithmParams.h"
#include "third_party/WebKit/public/platform/WebCryptoKeyAlgorithm.h"
#include "third_party/re2/re2/re2.h"

#if !defined(USE_OPENSSL)
#include <nss.h>
#include <pk11pub.h>

#include "crypto/nss_util.h"
#include "crypto/scoped_nss_types.h"
#endif

namespace webcrypto {

// static
void WebCryptoTestBase::SetUpTestCase() {
  test_runner::EnsureBlinkInitialized();
}

void PrintTo(const Status& status, ::std::ostream* os) {
  *os << StatusToString(status);
}

bool operator==(const Status& a, const Status& b) {
  if (a.IsSuccess() != b.IsSuccess())
    return false;
  if (a.IsSuccess())
    return true;
  return a.error_type() == b.error_type() &&
         a.error_details() == b.error_details();
}

bool operator!=(const Status& a, const Status& b) {
  return !(a == b);
}

void PrintTo(const CryptoData& data, ::std::ostream* os) {
  *os << "[" << base::HexEncode(data.bytes(), data.byte_length()) << "]";
}

bool operator==(const CryptoData& a, const CryptoData& b) {
  return a.byte_length() == b.byte_length() &&
         memcmp(a.bytes(), b.bytes(), a.byte_length()) == 0;
}

bool operator!=(const CryptoData& a, const CryptoData& b) {
  return !(a == b);
}

static std::string ErrorTypeToString(blink::WebCryptoErrorType type) {
  switch (type) {
    case blink::WebCryptoErrorTypeNotSupported:
      return "NotSupported";
    case blink::WebCryptoErrorTypeType:
      return "TypeError";
    case blink::WebCryptoErrorTypeData:
      return "DataError";
    case blink::WebCryptoErrorTypeSyntax:
      return "SyntaxError";
    case blink::WebCryptoErrorTypeOperation:
      return "OperationError";
    case blink::WebCryptoErrorTypeInvalidAccess:
      return "InvalidAccess";
    default:
      return "?";
  }
}

std::string StatusToString(const Status& status) {
  if (status.IsSuccess())
    return "Success";

  std::string result = ErrorTypeToString(status.error_type());
  if (!status.error_details().empty())
    result += ": " + status.error_details();
  return result;
}

bool SupportsAesGcm() {
  std::vector<uint8_t> key_raw(16, 0);

  blink::WebCryptoKey key;
  Status status = ImportKey(blink::WebCryptoKeyFormatRaw, CryptoData(key_raw),
                            CreateAlgorithm(blink::WebCryptoAlgorithmIdAesGcm),
                            true, blink::WebCryptoKeyUsageEncrypt, &key);

  if (status.IsError())
    EXPECT_EQ(blink::WebCryptoErrorTypeNotSupported, status.error_type());
  return status.IsSuccess();
}

bool SupportsRsaOaep() {
#if defined(USE_OPENSSL)
  return true;
#else
  crypto::EnsureNSSInit();
// TODO(eroman): Exclude version test for OS_CHROMEOS
#if defined(USE_NSS_CERTS)
  if (!NSS_VersionCheck("3.16.2"))
    return false;
#endif
  crypto::ScopedPK11Slot slot(PK11_GetInternalKeySlot());
  return !!PK11_DoesMechanism(slot.get(), CKM_RSA_PKCS_OAEP);
#endif
}

bool SupportsRsaPrivateKeyImport() {
// TODO(eroman): Exclude version test for OS_CHROMEOS
#if !defined(USE_OPENSSL) && defined(USE_NSS_CERTS)
  crypto::EnsureNSSInit();
  if (!NSS_VersionCheck("3.16.2")) {
    LOG(WARNING) << "RSA key import is not supported by this version of NSS. "
                    "Skipping some tests";
    return false;
  }
#endif
  return true;
}

blink::WebCryptoAlgorithm CreateRsaHashedKeyGenAlgorithm(
    blink::WebCryptoAlgorithmId algorithm_id,
    const blink::WebCryptoAlgorithmId hash_id,
    unsigned int modulus_length,
    const std::vector<uint8_t>& public_exponent) {
  DCHECK(blink::WebCryptoAlgorithm::isHash(hash_id));
  return blink::WebCryptoAlgorithm::adoptParamsAndCreate(
      algorithm_id, new blink::WebCryptoRsaHashedKeyGenParams(
                        CreateAlgorithm(hash_id), modulus_length,
                        vector_as_array(&public_exponent),
                        static_cast<unsigned int>(public_exponent.size())));
}

std::vector<uint8_t> Corrupted(const std::vector<uint8_t>& input) {
  std::vector<uint8_t> corrupted_data(input);
  if (corrupted_data.empty())
    corrupted_data.push_back(0);
  corrupted_data[corrupted_data.size() / 2] ^= 0x01;
  return corrupted_data;
}

std::vector<uint8_t> HexStringToBytes(const std::string& hex) {
  std::vector<uint8_t> bytes;
  base::HexStringToBytes(hex, &bytes);
  return bytes;
}

std::vector<uint8_t> MakeJsonVector(const std::string& json_string) {
  return std::vector<uint8_t>(json_string.begin(), json_string.end());
}

std::vector<uint8_t> MakeJsonVector(const base::DictionaryValue& dict) {
  std::string json;
  base::JSONWriter::Write(dict, &json);
  return MakeJsonVector(json);
}

::testing::AssertionResult ReadJsonTestFile(const char* test_file_name,
                                            scoped_ptr<base::Value>* value) {
  base::FilePath test_data_dir;
  if (!PathService::Get(base::DIR_SOURCE_ROOT, &test_data_dir))
    return ::testing::AssertionFailure() << "Couldn't retrieve test dir";

  base::FilePath file_path = test_data_dir.AppendASCII("components")
                                 .AppendASCII("test")
                                 .AppendASCII("data")
                                 .AppendASCII("webcrypto")
                                 .AppendASCII(test_file_name);

  std::string file_contents;
  if (!base::ReadFileToString(file_path, &file_contents)) {
    return ::testing::AssertionFailure()
           << "Couldn't read test file: " << file_path.value();
  }

  // Strip C++ style comments out of the "json" file, otherwise it cannot be
  // parsed.
  re2::RE2::GlobalReplace(&file_contents, re2::RE2("\\s*//.*"), "");

  // Parse the JSON to a dictionary.
  *value = base::JSONReader::Read(file_contents);
  if (!value->get()) {
    return ::testing::AssertionFailure()
           << "Couldn't parse test file JSON: " << file_path.value();
  }

  return ::testing::AssertionSuccess();
}

::testing::AssertionResult ReadJsonTestFileToList(
    const char* test_file_name,
    scoped_ptr<base::ListValue>* list) {
  // Read the JSON.
  scoped_ptr<base::Value> json;
  ::testing::AssertionResult result = ReadJsonTestFile(test_file_name, &json);
  if (!result)
    return result;

  // Cast to an ListValue.
  base::ListValue* list_value = NULL;
  if (!json->GetAsList(&list_value) || !list_value)
    return ::testing::AssertionFailure() << "The JSON was not a list";

  list->reset(list_value);
  ignore_result(json.release());

  return ::testing::AssertionSuccess();
}

::testing::AssertionResult ReadJsonTestFileToDictionary(
    const char* test_file_name,
    scoped_ptr<base::DictionaryValue>* dict) {
  // Read the JSON.
  scoped_ptr<base::Value> json;
  ::testing::AssertionResult result = ReadJsonTestFile(test_file_name, &json);
  if (!result)
    return result;

  // Cast to an DictionaryValue.
  base::DictionaryValue* dict_value = NULL;
  if (!json->GetAsDictionary(&dict_value) || !dict_value)
    return ::testing::AssertionFailure() << "The JSON was not a dictionary";

  dict->reset(dict_value);
  ignore_result(json.release());

  return ::testing::AssertionSuccess();
}

std::vector<uint8_t> GetBytesFromHexString(const base::DictionaryValue* dict,
                                           const std::string& property_name) {
  std::string hex_string;
  if (!dict->GetString(property_name, &hex_string)) {
    ADD_FAILURE() << "Couldn't get string property: " << property_name;
    return std::vector<uint8_t>();
  }

  return HexStringToBytes(hex_string);
}

blink::WebCryptoAlgorithm GetDigestAlgorithm(const base::DictionaryValue* dict,
                                             const char* property_name) {
  std::string algorithm_name;
  if (!dict->GetString(property_name, &algorithm_name)) {
    ADD_FAILURE() << "Couldn't get string property: " << property_name;
    return blink::WebCryptoAlgorithm::createNull();
  }

  struct {
    const char* name;
    blink::WebCryptoAlgorithmId id;
  } kDigestNameToId[] = {
      {"sha-1", blink::WebCryptoAlgorithmIdSha1},
      {"sha-256", blink::WebCryptoAlgorithmIdSha256},
      {"sha-384", blink::WebCryptoAlgorithmIdSha384},
      {"sha-512", blink::WebCryptoAlgorithmIdSha512},
  };

  for (size_t i = 0; i < arraysize(kDigestNameToId); ++i) {
    if (kDigestNameToId[i].name == algorithm_name)
      return CreateAlgorithm(kDigestNameToId[i].id);
  }

  return blink::WebCryptoAlgorithm::createNull();
}

// Creates a comparator for |bufs| which operates on indices rather than values.
class CompareUsingIndex {
 public:
  explicit CompareUsingIndex(const std::vector<std::vector<uint8_t>>* bufs)
      : bufs_(bufs) {}

  bool operator()(size_t i1, size_t i2) { return (*bufs_)[i1] < (*bufs_)[i2]; }

 private:
  const std::vector<std::vector<uint8_t>>* bufs_;
};

bool CopiesExist(const std::vector<std::vector<uint8_t>>& bufs) {
  // Sort the indices of |bufs| into a separate vector. This reduces the amount
  // of data copied versus sorting |bufs| directly.
  std::vector<size_t> sorted_indices(bufs.size());
  for (size_t i = 0; i < sorted_indices.size(); ++i)
    sorted_indices[i] = i;
  std::sort(sorted_indices.begin(), sorted_indices.end(),
            CompareUsingIndex(&bufs));

  // Scan for adjacent duplicates.
  for (size_t i = 1; i < sorted_indices.size(); ++i) {
    if (bufs[sorted_indices[i]] == bufs[sorted_indices[i - 1]])
      return true;
  }
  return false;
}

blink::WebCryptoAlgorithm CreateAesKeyGenAlgorithm(
    blink::WebCryptoAlgorithmId aes_alg_id,
    unsigned short length) {
  return blink::WebCryptoAlgorithm::adoptParamsAndCreate(
      aes_alg_id, new blink::WebCryptoAesKeyGenParams(length));
}

// The following key pair is comprised of the SPKI (public key) and PKCS#8
// (private key) representations of the key pair provided in Example 1 of the
// NIST test vectors at
// ftp://ftp.rsa.com/pub/rsalabs/tmp/pkcs1v15sign-vectors.txt
const unsigned int kModulusLengthBits = 1024;
const char* const kPublicKeySpkiDerHex =
    "30819f300d06092a864886f70d010101050003818d0030818902818100a5"
    "6e4a0e701017589a5187dc7ea841d156f2ec0e36ad52a44dfeb1e61f7ad9"
    "91d8c51056ffedb162b4c0f283a12a88a394dff526ab7291cbb307ceabfc"
    "e0b1dfd5cd9508096d5b2b8b6df5d671ef6377c0921cb23c270a70e2598e"
    "6ff89d19f105acc2d3f0cb35f29280e1386b6f64c4ef22e1e1f20d0ce8cf"
    "fb2249bd9a21370203010001";
const char* const kPrivateKeyPkcs8DerHex =
    "30820275020100300d06092a864886f70d01010105000482025f3082025b"
    "02010002818100a56e4a0e701017589a5187dc7ea841d156f2ec0e36ad52"
    "a44dfeb1e61f7ad991d8c51056ffedb162b4c0f283a12a88a394dff526ab"
    "7291cbb307ceabfce0b1dfd5cd9508096d5b2b8b6df5d671ef6377c0921c"
    "b23c270a70e2598e6ff89d19f105acc2d3f0cb35f29280e1386b6f64c4ef"
    "22e1e1f20d0ce8cffb2249bd9a2137020301000102818033a5042a90b27d"
    "4f5451ca9bbbd0b44771a101af884340aef9885f2a4bbe92e894a724ac3c"
    "568c8f97853ad07c0266c8c6a3ca0929f1e8f11231884429fc4d9ae55fee"
    "896a10ce707c3ed7e734e44727a39574501a532683109c2abacaba283c31"
    "b4bd2f53c3ee37e352cee34f9e503bd80c0622ad79c6dcee883547c6a3b3"
    "25024100e7e8942720a877517273a356053ea2a1bc0c94aa72d55c6e8629"
    "6b2dfc967948c0a72cbccca7eacb35706e09a1df55a1535bd9b3cc34160b"
    "3b6dcd3eda8e6443024100b69dca1cf7d4d7ec81e75b90fcca874abcde12"
    "3fd2700180aa90479b6e48de8d67ed24f9f19d85ba275874f542cd20dc72"
    "3e6963364a1f9425452b269a6799fd024028fa13938655be1f8a159cbaca"
    "5a72ea190c30089e19cd274a556f36c4f6e19f554b34c077790427bbdd8d"
    "d3ede2448328f385d81b30e8e43b2fffa02786197902401a8b38f398fa71"
    "2049898d7fb79ee0a77668791299cdfa09efc0e507acb21ed74301ef5bfd"
    "48be455eaeb6e1678255827580a8e4e8e14151d1510a82a3f2e729024027"
    "156aba4126d24a81f3a528cbfb27f56886f840a9f6e86e17a44b94fe9319"
    "584b8e22fdde1e5a2e3bd8aa5ba8d8584194eb2190acf832b847f13a3d24"
    "a79f4d";
// The modulus and exponent (in hex) of kPublicKeySpkiDerHex
const char* const kPublicKeyModulusHex =
    "A56E4A0E701017589A5187DC7EA841D156F2EC0E36AD52A44DFEB1E61F7AD991D8C51056"
    "FFEDB162B4C0F283A12A88A394DFF526AB7291CBB307CEABFCE0B1DFD5CD9508096D5B2B"
    "8B6DF5D671EF6377C0921CB23C270A70E2598E6FF89D19F105ACC2D3F0CB35F29280E138"
    "6B6F64C4EF22E1E1F20D0CE8CFFB2249BD9A2137";
const char* const kPublicKeyExponentHex = "010001";

blink::WebCryptoKey ImportSecretKeyFromRaw(
    const std::vector<uint8_t>& key_raw,
    const blink::WebCryptoAlgorithm& algorithm,
    blink::WebCryptoKeyUsageMask usage) {
  blink::WebCryptoKey key;
  bool extractable = true;
  EXPECT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatRaw, CryptoData(key_raw),
                      algorithm, extractable, usage, &key));

  EXPECT_FALSE(key.isNull());
  EXPECT_TRUE(key.handle());
  EXPECT_EQ(blink::WebCryptoKeyTypeSecret, key.type());
  EXPECT_EQ(algorithm.id(), key.algorithm().id());
  EXPECT_EQ(extractable, key.extractable());
  EXPECT_EQ(usage, key.usages());
  return key;
}

void ImportRsaKeyPair(const std::vector<uint8_t>& spki_der,
                      const std::vector<uint8_t>& pkcs8_der,
                      const blink::WebCryptoAlgorithm& algorithm,
                      bool extractable,
                      blink::WebCryptoKeyUsageMask public_key_usages,
                      blink::WebCryptoKeyUsageMask private_key_usages,
                      blink::WebCryptoKey* public_key,
                      blink::WebCryptoKey* private_key) {
  ASSERT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatSpki, CryptoData(spki_der),
                      algorithm, true, public_key_usages, public_key));
  EXPECT_FALSE(public_key->isNull());
  EXPECT_TRUE(public_key->handle());
  EXPECT_EQ(blink::WebCryptoKeyTypePublic, public_key->type());
  EXPECT_EQ(algorithm.id(), public_key->algorithm().id());
  EXPECT_TRUE(public_key->extractable());
  EXPECT_EQ(public_key_usages, public_key->usages());

  ASSERT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatPkcs8, CryptoData(pkcs8_der),
                      algorithm, extractable, private_key_usages, private_key));
  EXPECT_FALSE(private_key->isNull());
  EXPECT_TRUE(private_key->handle());
  EXPECT_EQ(blink::WebCryptoKeyTypePrivate, private_key->type());
  EXPECT_EQ(algorithm.id(), private_key->algorithm().id());
  EXPECT_EQ(extractable, private_key->extractable());
  EXPECT_EQ(private_key_usages, private_key->usages());
}

Status ImportKeyJwkFromDict(const base::DictionaryValue& dict,
                            const blink::WebCryptoAlgorithm& algorithm,
                            bool extractable,
                            blink::WebCryptoKeyUsageMask usages,
                            blink::WebCryptoKey* key) {
  return ImportKey(blink::WebCryptoKeyFormatJwk,
                   CryptoData(MakeJsonVector(dict)), algorithm, extractable,
                   usages, key);
}

scoped_ptr<base::DictionaryValue> GetJwkDictionary(
    const std::vector<uint8_t>& json) {
  base::StringPiece json_string(
      reinterpret_cast<const char*>(vector_as_array(&json)), json.size());
  scoped_ptr<base::Value> value = base::JSONReader::Read(json_string);
  EXPECT_TRUE(value.get());
  EXPECT_TRUE(value->IsType(base::Value::TYPE_DICTIONARY));

  return scoped_ptr<base::DictionaryValue>(
      static_cast<base::DictionaryValue*>(value.release()));
}

// Verifies the input dictionary contains the expected values. Exact matches are
// required on the fields examined.
::testing::AssertionResult VerifyJwk(
    const scoped_ptr<base::DictionaryValue>& dict,
    const std::string& kty_expected,
    const std::string& alg_expected,
    blink::WebCryptoKeyUsageMask use_mask_expected) {
  // ---- kty
  std::string value_string;
  if (!dict->GetString("kty", &value_string))
    return ::testing::AssertionFailure() << "Missing 'kty'";
  if (value_string != kty_expected)
    return ::testing::AssertionFailure() << "Expected 'kty' to be "
                                         << kty_expected << "but found "
                                         << value_string;

  // ---- alg
  if (!dict->GetString("alg", &value_string))
    return ::testing::AssertionFailure() << "Missing 'alg'";
  if (value_string != alg_expected)
    return ::testing::AssertionFailure() << "Expected 'alg' to be "
                                         << alg_expected << " but found "
                                         << value_string;

  // ---- ext
  // always expect ext == true in this case
  bool ext_value;
  if (!dict->GetBoolean("ext", &ext_value))
    return ::testing::AssertionFailure() << "Missing 'ext'";
  if (!ext_value)
    return ::testing::AssertionFailure()
           << "Expected 'ext' to be true but found false";

  // ---- key_ops
  base::ListValue* key_ops;
  if (!dict->GetList("key_ops", &key_ops))
    return ::testing::AssertionFailure() << "Missing 'key_ops'";
  blink::WebCryptoKeyUsageMask key_ops_mask = 0;
  Status status =
      GetWebCryptoUsagesFromJwkKeyOpsForTest(key_ops, &key_ops_mask);
  if (status.IsError())
    return ::testing::AssertionFailure() << "Failure extracting 'key_ops'";
  if (key_ops_mask != use_mask_expected)
    return ::testing::AssertionFailure()
           << "Expected 'key_ops' mask to be " << use_mask_expected
           << " but found " << key_ops_mask << " (" << value_string << ")";

  return ::testing::AssertionSuccess();
}

::testing::AssertionResult VerifySecretJwk(
    const std::vector<uint8_t>& json,
    const std::string& alg_expected,
    const std::string& k_expected_hex,
    blink::WebCryptoKeyUsageMask use_mask_expected) {
  scoped_ptr<base::DictionaryValue> dict = GetJwkDictionary(json);
  if (!dict.get() || dict->empty())
    return ::testing::AssertionFailure() << "JSON parsing failed";

  // ---- k
  std::string value_string;
  if (!dict->GetString("k", &value_string))
    return ::testing::AssertionFailure() << "Missing 'k'";
  std::string k_value;
  if (!Base64DecodeUrlSafe(value_string, &k_value))
    return ::testing::AssertionFailure() << "Base64DecodeUrlSafe(k) failed";
  if (!base::LowerCaseEqualsASCII(
          base::HexEncode(k_value.data(), k_value.size()),
          k_expected_hex.c_str())) {
    return ::testing::AssertionFailure() << "Expected 'k' to be "
                                         << k_expected_hex
                                         << " but found something different";
  }

  return VerifyJwk(dict, "oct", alg_expected, use_mask_expected);
}

::testing::AssertionResult VerifyPublicJwk(
    const std::vector<uint8_t>& json,
    const std::string& alg_expected,
    const std::string& n_expected_hex,
    const std::string& e_expected_hex,
    blink::WebCryptoKeyUsageMask use_mask_expected) {
  scoped_ptr<base::DictionaryValue> dict = GetJwkDictionary(json);
  if (!dict.get() || dict->empty())
    return ::testing::AssertionFailure() << "JSON parsing failed";

  // ---- n
  std::string value_string;
  if (!dict->GetString("n", &value_string))
    return ::testing::AssertionFailure() << "Missing 'n'";
  std::string n_value;
  if (!Base64DecodeUrlSafe(value_string, &n_value))
    return ::testing::AssertionFailure() << "Base64DecodeUrlSafe(n) failed";
  if (base::HexEncode(n_value.data(), n_value.size()) != n_expected_hex) {
    return ::testing::AssertionFailure() << "'n' does not match the expected "
                                            "value";
  }
  // TODO(padolph): LowerCaseEqualsASCII() does not work for above!

  // ---- e
  if (!dict->GetString("e", &value_string))
    return ::testing::AssertionFailure() << "Missing 'e'";
  std::string e_value;
  if (!Base64DecodeUrlSafe(value_string, &e_value))
    return ::testing::AssertionFailure() << "Base64DecodeUrlSafe(e) failed";
  if (!base::LowerCaseEqualsASCII(
          base::HexEncode(e_value.data(), e_value.size()),
          e_expected_hex.c_str())) {
    return ::testing::AssertionFailure() << "Expected 'e' to be "
                                         << e_expected_hex
                                         << " but found something different";
  }

  return VerifyJwk(dict, "RSA", alg_expected, use_mask_expected);
}

void ImportExportJwkSymmetricKey(
    int key_len_bits,
    const blink::WebCryptoAlgorithm& import_algorithm,
    blink::WebCryptoKeyUsageMask usages,
    const std::string& jwk_alg) {
  std::vector<uint8_t> json;
  std::string key_hex;

  // Hardcoded pseudo-random bytes to use for keys of different lengths.
  switch (key_len_bits) {
    case 128:
      key_hex = "3f1e7cd4f6f8543f6b1e16002e688623";
      break;
    case 256:
      key_hex =
          "bd08286b81a74783fd1ccf46b7e05af84ee25ae021210074159e0c4d9d907692";
      break;
    case 384:
      key_hex =
          "a22c5441c8b185602283d64c7221de1d0951e706bfc09539435ec0e0ed614e1d40"
          "6623f2b31d31819fec30993380dd82";
      break;
    case 512:
      key_hex =
          "5834f639000d4cf82de124fbfd26fb88d463e99f839a76ba41ac88967c80a3f61e"
          "1239a452e573dba0750e988152988576efd75b8d0229b7aca2ada2afd392ee";
      break;
    default:
      FAIL() << "Unexpected key_len_bits" << key_len_bits;
  }

  // Import a raw key.
  blink::WebCryptoKey key = ImportSecretKeyFromRaw(HexStringToBytes(key_hex),
                                                   import_algorithm, usages);

  // Export the key in JWK format and validate.
  ASSERT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatJwk, key, &json));
  EXPECT_TRUE(VerifySecretJwk(json, jwk_alg, key_hex, usages));

  // Import the JWK-formatted key.
  ASSERT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatJwk, CryptoData(json),
                      import_algorithm, true, usages, &key));
  EXPECT_TRUE(key.handle());
  EXPECT_EQ(blink::WebCryptoKeyTypeSecret, key.type());
  EXPECT_EQ(import_algorithm.id(), key.algorithm().id());
  EXPECT_EQ(true, key.extractable());
  EXPECT_EQ(usages, key.usages());

  // Export the key in raw format and compare to the original.
  std::vector<uint8_t> key_raw_out;
  ASSERT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatRaw, key, &key_raw_out));
  EXPECT_BYTES_EQ_HEX(key_hex, key_raw_out);
}

Status GenerateSecretKey(const blink::WebCryptoAlgorithm& algorithm,
                         bool extractable,
                         blink::WebCryptoKeyUsageMask usages,
                         blink::WebCryptoKey* key) {
  GenerateKeyResult result;
  Status status = GenerateKey(algorithm, extractable, usages, &result);
  if (status.IsError())
    return status;

  if (result.type() != GenerateKeyResult::TYPE_SECRET_KEY)
    return Status::ErrorUnexpected();

  *key = result.secret_key();

  return Status::Success();
}

Status GenerateKeyPair(const blink::WebCryptoAlgorithm& algorithm,
                       bool extractable,
                       blink::WebCryptoKeyUsageMask usages,
                       blink::WebCryptoKey* public_key,
                       blink::WebCryptoKey* private_key) {
  GenerateKeyResult result;
  Status status = GenerateKey(algorithm, extractable, usages, &result);
  if (status.IsError())
    return status;

  if (result.type() != GenerateKeyResult::TYPE_PUBLIC_PRIVATE_KEY_PAIR)
    return Status::ErrorUnexpected();

  *public_key = result.public_key();
  *private_key = result.private_key();

  return Status::Success();
}

blink::WebCryptoKeyFormat GetKeyFormatFromJsonTestCase(
    const base::DictionaryValue* test) {
  std::string format;
  EXPECT_TRUE(test->GetString("key_format", &format));
  if (format == "jwk")
    return blink::WebCryptoKeyFormatJwk;
  else if (format == "pkcs8")
    return blink::WebCryptoKeyFormatPkcs8;
  else if (format == "spki")
    return blink::WebCryptoKeyFormatSpki;
  else if (format == "raw")
    return blink::WebCryptoKeyFormatRaw;

  ADD_FAILURE() << "Unrecognized key format: " << format;
  return blink::WebCryptoKeyFormatRaw;
}

std::vector<uint8_t> GetKeyDataFromJsonTestCase(
    const base::DictionaryValue* test,
    blink::WebCryptoKeyFormat key_format) {
  if (key_format == blink::WebCryptoKeyFormatJwk) {
    const base::DictionaryValue* json;
    EXPECT_TRUE(test->GetDictionary("key", &json));
    return MakeJsonVector(*json);
  }
  return GetBytesFromHexString(test, "key");
}

blink::WebCryptoNamedCurve GetCurveNameFromDictionary(
    const base::DictionaryValue* dict) {
  std::string curve_str;
  if (!dict->GetString("crv", &curve_str)) {
    ADD_FAILURE() << "Missing crv parameter";
    return blink::WebCryptoNamedCurveP384;
  }

  if (curve_str == "P-256")
    return blink::WebCryptoNamedCurveP256;
  if (curve_str == "P-384")
    return blink::WebCryptoNamedCurveP384;
  if (curve_str == "P-521")
    return blink::WebCryptoNamedCurveP521;
  else
    ADD_FAILURE() << "Unrecognized curve name: " << curve_str;

  return blink::WebCryptoNamedCurveP384;
}

}  // namespace webcrypto

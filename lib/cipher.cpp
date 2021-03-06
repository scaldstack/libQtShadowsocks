/*
 * cipher.cpp - the source file of Cipher class
 *
 * Copyright (C) 2014-2017 Symeon Huang <hzwhuang@gmail.com>
 *
 * This file is part of the libQtShadowsocks.
 *
 * libQtShadowsocks is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * libQtShadowsocks is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libQtShadowsocks; see the file LICENSE. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "cipher.h"

#include <botan/auto_rng.h>
#include <botan/key_filt.h>
#include <botan/lookup.h>
#include <botan/pipe.h>
#include <botan/md5.h>

#ifdef USE_BOTAN2
#include <botan/hkdf.h>
#include <botan/hmac.h>
#include <botan/sha160.h>
#endif

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <stdexcept>
#include <memory>

using namespace QSS;

#ifdef USE_BOTAN2
typedef Botan::secure_vector<Botan::byte> SecureByteArray;
#define DataOfSecureByteArray(sba) sba.data()
#else
typedef Botan::SecureVector<Botan::byte> SecureByteArray;
#define DataOfSecureByteArray(sba) sba.begin()
#endif

Cipher::Cipher(const std::string &method,
               const std::string &psKey,
               const std::string &iv,
               bool encrypt,
               QObject *parent) :
    QObject(parent),
    key(psKey),
    iv(iv),
    cipherInfo(cipherInfoMap.at(method))
{
    if (method.find("rc4") != std::string::npos) {
        rc4.reset(new RC4(key, iv));
        return;
    }
#ifndef USE_BOTAN2
    else if (method.find("chacha20") != std::string::npos) {
        chacha.reset(new ChaCha(key, iv));
        return;
    }
#endif
    try {
#ifdef USE_BOTAN2
        if (cipherInfoMap.at(method).type == CipherType::AEAD) {
            // Initialises necessary class members for AEAD ciphers
            msgHashFunc.reset(new Botan::SHA_160()); // SHA1
            msgAuthCode.reset(new Botan::HMAC(msgHashFunc.get()));
            kdf.reset(new Botan::HKDF(msgAuthCode.get()));
        }
#endif

        Botan::SymmetricKey _key(
                    reinterpret_cast<const Botan::byte *>(key.data()),
                    key.size());
        Botan::InitializationVector _iv(
                    reinterpret_cast<const Botan::byte *>(iv.data()),
                    iv.size());
        Botan::Keyed_Filter *filter = Botan::get_cipher(cipherInfo.internalName, _key, _iv,
                    encrypt ? Botan::ENCRYPTION : Botan::DECRYPTION);
        // Botan::pipe will take control over filter
        // we shouldn't deallocate filter externally
        pipe.reset(new Botan::Pipe(filter));
    } catch(const Botan::Exception &e) {
        qFatal("%s\n", e.what());
    }
}

Cipher::~Cipher()
{
}

const std::map<std::string, Cipher::CipherInfo> Cipher::cipherInfoMap = {
    {"aes-128-cfb", {"AES-128/CFB", 16, 16, Cipher::CipherType::STREAM}},
    {"aes-192-cfb", {"AES-192/CFB", 24, 16, Cipher::CipherType::STREAM}},
    {"aes-256-cfb", {"AES-256/CFB", 32, 16, Cipher::CipherType::STREAM}},
    {"aes-128-ctr", {"AES-128/CTR-BE", 16, 16, Cipher::CipherType::STREAM}},
    {"aes-192-ctr", {"AES-192/CTR-BE", 24, 16, Cipher::CipherType::STREAM}},
    {"aes-256-ctr", {"AES-256/CTR-BE", 32, 16, Cipher::CipherType::STREAM}},
    {"bf-cfb", {"Blowfish/CFB", 16, 8, Cipher::CipherType::STREAM}},
    {"camellia-128-cfb", {"Camellia-128/CFB", 16, 16, Cipher::CipherType::STREAM}},
    {"camellia-192-cfb", {"Camellia-192/CFB", 24, 16, Cipher::CipherType::STREAM}},
    {"camellia-256-cfb", {"Camellia-256/CFB", 32, 16, Cipher::CipherType::STREAM}},
    {"cast5-cfb", {"CAST-128/CFB", 16, 8, Cipher::CipherType::STREAM}},
    {"chacha20", {"ChaCha", 32, 8, Cipher::CipherType::STREAM}},
    {"chacha20-ietf", {"ChaCha", 32, 12, Cipher::CipherType::STREAM}},
    {"des-cfb", {"DES/CFB", 8, 8, Cipher::CipherType::STREAM}},
    {"idea-cfb", {"IDEA/CFB", 16, 8, Cipher::CipherType::STREAM}},
    {"rc2-cfb", {"RC2/CFB", 16, 8, Cipher::CipherType::STREAM}},
    {"rc4-md5", {"RC4-MD5", 16, 16, Cipher::CipherType::STREAM}},
    {"salsa20", {"Salsa20", 32, 8, Cipher::CipherType::STREAM}},
    {"seed-cfb", {"SEED/CFB", 16, 16, Cipher::CipherType::STREAM}},
    {"serpent-256-cfb", {"Serpent/CFB", 32, 16, Cipher::CipherType::STREAM}}
#ifdef USE_BOTAN2
   ,{"aes-256-gcm", {"AES-128/GCM", 32, 12, Cipher::CipherType::AEAD, 32, 16}}
#endif
};
const std::string Cipher::kdfLabel = {"ss-subkey"};
const int Cipher::AUTH_LEN = 10;

std::string Cipher::update(const std::string &data)
{
    if (chacha) {
        return chacha->update(data.data(), data.size());
    } else if (rc4) {
        return rc4->update(data);
    } else if (pipe) {
        pipe->process_msg(reinterpret_cast<const Botan::byte *>
                          (data.data()), data.size());
        SecureByteArray c = pipe->read_all(Botan::Pipe::LAST_MESSAGE);
        return std::string(reinterpret_cast<const char *>(DataOfSecureByteArray(c)),
                           c.size());
    } else {
        throw std::logic_error("Underlying ciphers are all uninitialised!");
    }
}

const std::string &Cipher::getIV() const
{
    return iv;
}

std::string Cipher::randomIv(int length)
{
    //directly return empty byte array if no need to genenrate iv
    if (length == 0) {
        return std::string();
    }

    Botan::AutoSeeded_RNG rng;
    SecureByteArray out = rng.random_vec(length);
    return std::string(reinterpret_cast<const char *>(DataOfSecureByteArray(out)), out.size());
}

std::string Cipher::randomIv(const std::string &method)
{
    return randomIv(cipherInfoMap.at(method).ivLen);
}

std::string Cipher::hmacSha1(const std::string &key, const std::string &msg)
{
    QByteArray result = QMessageAuthenticationCode::hash(QByteArray(msg.data(), msg.size()),
                                           QByteArray(key.data(), key.size()),
                                           QCryptographicHash::Sha1).left(AUTH_LEN);
    return std::string(result.data(), result.size());
}

std::string Cipher::md5Hash(const std::string &in)
{
    Botan::MD5 md5;
    SecureByteArray result = md5.process(in);
    return std::string(reinterpret_cast<const char*>(DataOfSecureByteArray(result)), result.size());
}

bool Cipher::isSupported(const std::string &method)
{
#ifndef USE_BOTAN2
    if (method.find("chacha20") != std::string::npos)  return true;
#endif

    if (method.find("rc4") == std::string::npos) {
        std::unique_ptr<Botan::Keyed_Filter> filter;
        try {
            filter.reset(Botan::get_cipher(method, Botan::ENCRYPTION));
        } catch (Botan::Exception &e) {
            qDebug("%s\n", e.what());
            return false;
        }
    }
    return true;
}

std::vector<std::string> Cipher::supportedMethods()
{
    std::vector<std::string> supportedMethods;
    for (auto& cipher : Cipher::cipherInfoMap) {
        if (Cipher::isSupported(cipher.second.internalName)) {
            supportedMethods.push_back(cipher.first);
        }
    }
    return supportedMethods;
}

#ifdef USE_BOTAN2
/*
 * Derives per-session subkey from the master key and IV, which is required
 * for Shadowsocks AEAD ciphers
 */
std::string Cipher::deriveSubkey() const
{
    Q_ASSERT(kdf);
    std::string salt = randomIv(cipherInfo.saltLen);
    SecureByteArray skey = kdf->derive_key(cipherInfo.keyLen, reinterpret_cast<const uint8_t*>(key.data()), key.length(), salt, kdfLabel);
    return std::string(reinterpret_cast<const char *>(DataOfSecureByteArray(skey)),
                       skey.size());
}
#endif

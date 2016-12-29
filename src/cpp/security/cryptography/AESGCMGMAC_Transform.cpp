// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*!
 * @file AESGCMGMAC_Transform.cpp
 */

#include "AESGCMGMAC_Transform.h"

#include <fastrtps/log/Log.h>

#include <openssl/aes.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <cstring>

#define RTPS_HEADER_SIZE 20

using namespace eprosima::fastrtps::rtps::security;

AESGCMGMAC_Transform::AESGCMGMAC_Transform(){}
AESGCMGMAC_Transform::~AESGCMGMAC_Transform(){}

bool AESGCMGMAC_Transform::encode_serialized_payload(
                std::vector<uint8_t> &encoded_buffer,
                std::vector<uint8_t> &extra_inline_qos,
                const std::vector<uint8_t> &plain_buffer,
                DatawriterCryptoHandle &sending_datawriter_crypto,
                SecurityException &exception){

    AESGCMGMAC_WriterCryptoHandle& local_writer = AESGCMGMAC_WriterCryptoHandle::narrow(sending_datawriter_crypto);
    if(local_writer.nil()){
        logWarning(SECURITY_CRYPTO,"Invalid CryptoHandle");
        return false;
    }

    //If the maximum number of blocks have been processed, generate a new SessionKey
    if(local_writer->session_block_counter >= local_writer->max_blocks_per_session){
        local_writer->session_id += 1;

        local_writer->SessionKey = compute_sessionkey(local_writer->WriterKeyMaterial.master_sender_key,
                local_writer->WriterKeyMaterial.master_salt,
                local_writer->session_id);

        //ReceiverSpecific keys shall be computed specifically when needed
        local_writer->session_block_counter = 0;
    }
    //In any case, increment session block counter
    local_writer->session_block_counter += 1;

    //Build NONCE elements (Build once, use once)
    uint64_t initialization_vector_suffix;  //iv suffix changes with every operation
    RAND_bytes( (unsigned char*)(&initialization_vector_suffix), sizeof(uint64_t) );
    std::array<uint8_t,12> initialization_vector; //96 bytes, session_id + suffix
    memcpy(initialization_vector.data(),&(local_writer->session_id),4);
    memcpy(initialization_vector.data()+4,&initialization_vector_suffix,8);

    //Build SecureDataHeader
    SecureDataHeader header;

    header.transform_identifier.transformation_kind = local_writer->WriterKeyMaterial.transformation_kind;
    header.transform_identifier.transformation_key_id = local_writer->WriterKeyMaterial.sender_key_id;
    memcpy( header.session_id.data(), &(local_writer->session_id), 4);
    memcpy( header.initialization_vector_suffix.data() , &initialization_vector_suffix, 8);

    //Cypher the plain rtps message -> SecureDataBody
    int rv = RAND_load_file("/dev/urandom", 32); //Init random number gen

    size_t enc_length = plain_buffer.size()*3;
    std::vector<uint8_t> output;
    output.resize(enc_length,0);

    unsigned char tag[AES_BLOCK_SIZE]; //Container for the Authentication Tag (will become common mac)

    int actual_size=0, final_size=0;
    EVP_CIPHER_CTX* e_ctx = EVP_CIPHER_CTX_new();
    if(local_writer->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES128_GCM)){
        EVP_EncryptInit(e_ctx, EVP_aes_128_gcm(), (const unsigned char*)(local_writer->SessionKey.data()), initialization_vector.data());
    }
    if(local_writer->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES256_GCM)){
        EVP_EncryptInit(e_ctx, EVP_aes_256_gcm(), (const unsigned char*)(local_writer->SessionKey.data()), initialization_vector.data());
    }
    EVP_EncryptUpdate(e_ctx, output.data(), &actual_size, (const unsigned char*)plain_buffer.data(), plain_buffer.size());
    EVP_EncryptFinal(e_ctx, output.data() + actual_size, &final_size);
    EVP_CIPHER_CTX_ctrl(e_ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
    output.resize(actual_size+final_size);
    EVP_CIPHER_CTX_free(e_ctx);

    //Copy the results into SecureDataBody
    SecureDataBody body;
    body.secure_data.resize(output.size());
    memcpy(body.secure_data.data(),output.data(),output.size());

    //Build Secure DataTag
    SecureDataTag dataTag;
    memcpy(dataTag.common_mac.data(),tag, 16);

    //Assemble the message
    encoded_buffer.clear();

    //Header
    std::vector<uint8_t> serialized_header = serialize_SecureDataHeader(header);
    //Body
    std::vector<uint8_t> serialized_body = serialize_SecureDataBody(body);
    //Tag
    std::vector<uint8_t> serialized_tag = serialize_SecureDataTag(dataTag);
    unsigned char flags = 0x00;
    encoded_buffer = assemble_serialized_payload(serialized_header, serialized_body, serialized_tag, flags);

    return true;
}

bool AESGCMGMAC_Transform::encode_datawriter_submessage(
                std::vector<uint8_t> &encoded_rtps_submessage,
                const std::vector<uint8_t> &plain_rtps_submessage,
                DatawriterCryptoHandle &sending_datawriter_crypto,
                std::vector<DatareaderCryptoHandle*> receiving_datareader_crypto_list,
                SecurityException &exception){

    AESGCMGMAC_WriterCryptoHandle& local_writer = AESGCMGMAC_WriterCryptoHandle::narrow(sending_datawriter_crypto);
    if(local_writer.nil()){
        logWarning(SECURITY_CRYPTO,"Invalid cryptoHandle");
        return false;
    }
    bool update_specific_keys = false;
    //If the maximum number of blocks have been processed, generate a new SessionKey
    if(local_writer->session_block_counter >= local_writer->max_blocks_per_session){
        local_writer->session_id += 1;
        update_specific_keys = true;
        local_writer->SessionKey = compute_sessionkey(local_writer->WriterKeyMaterial.master_sender_key,
                local_writer->WriterKeyMaterial.master_salt,
                local_writer->session_id);

        //ReceiverSpecific keys shall be computed specifically when needed
        local_writer->session_block_counter = 0;
    }

    local_writer->session_block_counter += 1;

    //Build remaining NONCE elements
    uint64_t initialization_vector_suffix;  //iv suffix changes with every operation
    RAND_bytes( (unsigned char*)(&initialization_vector_suffix), sizeof(uint64_t) );
    std::array<uint8_t,12> initialization_vector; //96 bytes, session_id + suffix
    memcpy(initialization_vector.data(),&(local_writer->session_id),4);
    memcpy(initialization_vector.data()+4,&initialization_vector_suffix,8);

    //Build SecureDataHeader
    SecureDataHeader header;

    header.transform_identifier.transformation_kind = local_writer->WriterKeyMaterial.transformation_kind;
    header.transform_identifier.transformation_key_id = local_writer->WriterKeyMaterial.sender_key_id;
    memcpy( header.session_id.data(), &(local_writer->session_id), 4);
    memcpy( header.initialization_vector_suffix.data() , &initialization_vector_suffix, 8);


    //Cypher the plain rtps message -> SecureDataBody
    int rv = RAND_load_file("/dev/urandom", 32); //Init random number gen

    size_t enc_length = plain_rtps_submessage.size()*3;
    std::vector<uint8_t> output;
    output.resize(enc_length,0);

    unsigned char tag[AES_BLOCK_SIZE]; //Container for the Authentication Tag (will become common mac)

    int actual_size=0, final_size=0;
    EVP_CIPHER_CTX* e_ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit(e_ctx, EVP_aes_128_gcm(), (const unsigned char*)(local_writer->SessionKey.data()), initialization_vector.data());
    EVP_EncryptUpdate(e_ctx, output.data(), &actual_size, (const unsigned char*)plain_rtps_submessage.data(), plain_rtps_submessage.size());
    EVP_EncryptFinal(e_ctx, output.data() + actual_size, &final_size);
    EVP_CIPHER_CTX_ctrl(e_ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
    output.resize(actual_size+final_size);
    EVP_CIPHER_CTX_free(e_ctx);

    //Copy the results into SecureDataBody
    SecureDataBody body;
    body.secure_data.resize(output.size());
    memcpy(body.secure_data.data(),output.data(),output.size());

    //Build Secure DataTag
    SecureDataTag dataTag;
    memcpy(dataTag.common_mac.data(),tag, 16);

    //Check the list of receivers, search for keys and compute session keys as needed
    for(auto rec = receiving_datareader_crypto_list.begin(); rec != receiving_datareader_crypto_list.end(); ++rec){

        AESGCMGMAC_ReaderCryptoHandle& remote_reader = AESGCMGMAC_ReaderCryptoHandle::narrow(**rec);
        if(remote_reader.nil()){
            logWarning(SECURITY_CRYPTO,"Invalid CryptoHandle");
            return false;
        }

        //Update the key if needed
        if(update_specific_keys){
            //Update triggered!
            remote_reader->session_id = local_writer->session_id;
            remote_reader->SessionKey = compute_sessionkey(remote_reader->Writer2ReaderKeyMaterial.at(0).master_receiver_specific_key,
                remote_reader->Writer2ReaderKeyMaterial.at(0).master_salt,
                remote_reader->session_id);
        }

        //Obtain MAC using ReceiverSpecificKey and the same Initialization Vector as before
        int actual_size=0, final_size=0;
        EVP_CIPHER_CTX* e_ctx = EVP_CIPHER_CTX_new();
        EVP_EncryptInit(e_ctx, EVP_aes_128_gcm(), (const unsigned char*)(remote_reader->SessionKey.data()), initialization_vector.data());
        EVP_EncryptUpdate(e_ctx, NULL, &actual_size, dataTag.common_mac.data(), 16);
        EVP_EncryptFinal(e_ctx, output.data() + actual_size, &final_size);
        EVP_CIPHER_CTX_ctrl(e_ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
        output.resize(actual_size+final_size);
        EVP_CIPHER_CTX_free(e_ctx);

        ReceiverSpecificMAC buffer;
        buffer.receiver_mac_key_id = remote_reader->Writer2ReaderKeyMaterial.at(0).receiver_specific_key_id;
        memcpy(buffer.receiver_mac.data(),tag,16);
        //Push the MAC into the dataTag
        dataTag.receiver_specific_macs.push_back(buffer);
    }

    //Assemble the message
    encoded_rtps_submessage.clear();

    //Header
    std::vector<uint8_t> serialized_header = serialize_SecureDataHeader(header);

    //Body
    std::vector<uint8_t> serialized_body = serialize_SecureDataBody(body);

    //Tag
    std::vector<uint8_t> serialized_tag = serialize_SecureDataTag(dataTag);
    //Flags
    unsigned char flags = 0x00;

    encoded_rtps_submessage = assemble_endpoint_submessage(serialized_header, serialized_body, serialized_tag, flags);

    return true;
}

bool AESGCMGMAC_Transform::encode_datareader_submessage(
                std::vector<uint8_t> &encoded_rtps_submessage,
                const std::vector<uint8_t> &plain_rtps_submessage,
                DatareaderCryptoHandle &sending_datareader_crypto,
                std::vector<DatawriterCryptoHandle*> &receiving_datawriter_crypto_list,
                SecurityException &exception){

    AESGCMGMAC_ReaderCryptoHandle& local_reader = AESGCMGMAC_ReaderCryptoHandle::narrow(sending_datareader_crypto);
    if(local_reader.nil()){
        logWarning(SECURITY_CRYPTO,"Invalid CryptoHandle");
        return false;
    }

    //Step 2 - If the maximum number of blocks have been processed, generate a new SessionKey
    bool update_specific_keys = false;
    if(local_reader->session_block_counter >= local_reader->max_blocks_per_session){
        local_reader->session_id += 1;
        update_specific_keys = true;
        local_reader->SessionKey = compute_sessionkey(local_reader->ReaderKeyMaterial.master_sender_key,
                local_reader->ReaderKeyMaterial.master_salt,
                local_reader->session_id);

        //ReceiverSpecific keys shall be computed specifically when needed
        local_reader->session_block_counter = 0;
    }

    local_reader->session_block_counter += 1;

    //Build remaining NONCE elements
    uint64_t initialization_vector_suffix;  //iv suffix changes with every operation
    RAND_bytes( (unsigned char*)(&initialization_vector_suffix), sizeof(uint64_t) );
    std::array<uint8_t,12> initialization_vector; //96 bytes, session_id + suffix
    memcpy(initialization_vector.data(),&(local_reader->session_id),4);
    memcpy(initialization_vector.data()+4,&initialization_vector_suffix,8);

    //Build SecureDataHeader
    SecureDataHeader header;

    header.transform_identifier.transformation_kind = local_reader->ReaderKeyMaterial.transformation_kind;
    header.transform_identifier.transformation_key_id = local_reader->ReaderKeyMaterial.sender_key_id;
    memcpy( header.session_id.data(), &(local_reader->session_id), 4);
    memcpy( header.initialization_vector_suffix.data() , &initialization_vector_suffix, 8);


    //Cypher the plain rtps message -> SecureDataBody
    int rv = RAND_load_file("/dev/urandom", 32); //Init random number gen

    size_t enc_length = plain_rtps_submessage.size()*3;
    std::vector<uint8_t> output;
    output.resize(enc_length,0);

    unsigned char tag[AES_BLOCK_SIZE]; //Container for the Authentication Tag (will become common mac)

    int actual_size=0, final_size=0;
    EVP_CIPHER_CTX* e_ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit(e_ctx, EVP_aes_128_gcm(), (const unsigned char*)(local_reader->SessionKey.data()), initialization_vector.data());
    EVP_EncryptUpdate(e_ctx, output.data(), &actual_size, (const unsigned char*)plain_rtps_submessage.data(), plain_rtps_submessage.size());
    EVP_EncryptFinal(e_ctx, output.data() + actual_size, &final_size);
    EVP_CIPHER_CTX_ctrl(e_ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
    output.resize(actual_size+final_size);
    EVP_CIPHER_CTX_free(e_ctx);

    //Copy the results into SecureDataBody
    SecureDataBody body;
    body.secure_data.resize(output.size());
    memcpy(body.secure_data.data(),output.data(),output.size());

    //Build Secure DataTag
    SecureDataTag dataTag;
    memcpy(dataTag.common_mac.data(),tag, 16);

    //Check the list of receivers, search for keys and compute session keys as needed
    for(auto rec = receiving_datawriter_crypto_list.begin(); rec != receiving_datawriter_crypto_list.end(); ++rec){

        AESGCMGMAC_WriterCryptoHandle& remote_writer = AESGCMGMAC_WriterCryptoHandle::narrow(**rec);
        if(remote_writer.nil()){
            //TODO (santi) Provide insight
            return false;
        }

        //Update the key if needed
        if(update_specific_keys){
            //Update triggered!
            remote_writer->session_id = local_reader->session_id;
            remote_writer->SessionKey = compute_sessionkey(remote_writer->Reader2WriterKeyMaterial.at(0).master_receiver_specific_key,
                remote_writer->Reader2WriterKeyMaterial.at(0).master_salt,
                remote_writer->session_id);
        }

        //Obtain MAC using ReceiverSpecificKey and the same Initialization Vector as before
        int actual_size=0, final_size=0;
        EVP_CIPHER_CTX* e_ctx = EVP_CIPHER_CTX_new();
        EVP_EncryptInit(e_ctx, EVP_aes_128_gcm(), (const unsigned char*)(remote_writer->SessionKey.data()), initialization_vector.data());
        EVP_EncryptUpdate(e_ctx, NULL, &actual_size, dataTag.common_mac.data(), 16);
        EVP_EncryptFinal(e_ctx, output.data() + actual_size, &final_size);
        EVP_CIPHER_CTX_ctrl(e_ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
        output.resize(actual_size+final_size);
        EVP_CIPHER_CTX_free(e_ctx);

        ReceiverSpecificMAC buffer;
        buffer.receiver_mac_key_id = remote_writer->Reader2WriterKeyMaterial.at(0).receiver_specific_key_id;
        memcpy(buffer.receiver_mac.data(),tag,16);
        //Push the MAC into the dataTag
        dataTag.receiver_specific_macs.push_back(buffer);
    }

    //Assemble the message
    encoded_rtps_submessage.clear();

    //Header
    std::vector<uint8_t> serialized_header = serialize_SecureDataHeader(header);
    //Body
    std::vector<uint8_t> serialized_body = serialize_SecureDataBody(body);
    //Tag
    std::vector<uint8_t> serialized_tag = serialize_SecureDataTag(dataTag);

    unsigned char flags = 0x00;

    encoded_rtps_submessage = assemble_endpoint_submessage(serialized_header, serialized_body, serialized_tag, flags);

    return true;
}

bool AESGCMGMAC_Transform::encode_rtps_message(
                std::vector<uint8_t> &encoded_rtps_message,
                const std::vector<uint8_t> &plain_rtps_message,
                ParticipantCryptoHandle &sending_crypto,
                const std::vector<ParticipantCryptoHandle*> &receiving_crypto_list,
                SecurityException &exception){

    AESGCMGMAC_ParticipantCryptoHandle& local_participant = AESGCMGMAC_ParticipantCryptoHandle::narrow(sending_crypto);

    if(local_participant.nil())
    {
        logError(SECURITY_CRYPTO,"Invalid CryptoToken");
        return false;
    }

    //Extract RTPS Header
    std::vector<uint8_t> rtps_header;
    for(int i=0;i<RTPS_HEADER_SIZE;i++) rtps_header.push_back(plain_rtps_message.at(i));
    //Extract Payload
    std::vector<uint8_t> payload;
    for(int i=RTPS_HEADER_SIZE;i<plain_rtps_message.size();i++) payload.push_back(plain_rtps_message.at(i));

    // If the maximum number of blocks have been processed, generate a new SessionKey
    bool update_specific_keys = false;
    if(local_participant->session_block_counter >= local_participant->max_blocks_per_session){
        local_participant->session_id += 1;
        update_specific_keys = true;
        local_participant->SessionKey = compute_sessionkey(local_participant->ParticipantKeyMaterial.master_sender_key,
                local_participant->ParticipantKeyMaterial.master_salt,
                local_participant->session_id);

        //ReceiverSpecific keys shall be computed specifically when needed
        local_participant->session_block_counter = 0;
        //Insert outdate session_id values in all RemoteParticipant trackers to trigger a SessionkeyUpdate
    }

    local_participant->session_block_counter += 1;

    //Build remaining NONCE elements
    uint64_t initialization_vector_suffix;  //iv suffix changes with every operation
    RAND_bytes( (unsigned char*)(&initialization_vector_suffix), sizeof(uint64_t) );
    std::array<uint8_t,12> initialization_vector; //96 bytes, session_id + suffix
    memcpy(initialization_vector.data(),&(local_participant->session_id),4);
    memcpy(initialization_vector.data()+4,&initialization_vector_suffix,8);

    //Build SecureDataHeader
    SecureDataHeader header;

    header.transform_identifier.transformation_kind = local_participant->ParticipantKeyMaterial.transformation_kind;
    header.transform_identifier.transformation_key_id = local_participant->ParticipantKeyMaterial.sender_key_id;
    memcpy( header.session_id.data(), &(local_participant->session_id), 4);
    memcpy( header.initialization_vector_suffix.data() , &initialization_vector_suffix, 8);


    //Cypher the plain rtps message -> SecureDataBody
    int rv = RAND_load_file("/dev/urandom", 32); //Init random number gen

    size_t enc_length = ( payload.size()) * 3;
    std::vector<uint8_t> output;
    output.resize(enc_length,0);

    unsigned char tag[AES_BLOCK_SIZE]; //Container for the Authentication Tag (will become common mac)

    int actual_size=0, final_size=0;
    EVP_CIPHER_CTX* e_ctx = EVP_CIPHER_CTX_new();
    if( (local_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES128_GCM)) |
        (local_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES128_GMAC)))
    {
        EVP_EncryptInit(e_ctx, EVP_aes_128_gcm(), (const unsigned char*)(local_participant->SessionKey.data()), initialization_vector.data());
    }
    if( (local_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES256_GCM)) |
        (local_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES256_GMAC))) 
    {
      EVP_EncryptInit(e_ctx, EVP_aes_256_gcm(), (const unsigned char*)(local_participant->SessionKey.data()), initialization_vector.data());
    }

    if( (local_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES128_GCM)) |
        (local_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES256_GCM)))
    {
        //We are in GCM mode: We need encryption and signature
        EVP_EncryptUpdate(e_ctx, output.data(), &actual_size, (const unsigned char*)payload.data(), payload.size());
        EVP_EncryptFinal(e_ctx, output.data() + actual_size, &final_size);
        EVP_CIPHER_CTX_ctrl(e_ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
        output.resize(actual_size+final_size);
    }else{  
        //We are in GMAC mode: We need a signature but no encryption is needed
        EVP_EncryptUpdate(e_ctx, NULL, &actual_size, (const unsigned char*)payload.data(), payload.size());
        EVP_EncryptFinal(e_ctx, output.data() + actual_size, &final_size);
        EVP_CIPHER_CTX_ctrl(e_ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
        output.resize(payload.size());
        memcpy(output.data(), payload.data(), payload.size());
    }
    EVP_CIPHER_CTX_free(e_ctx);

    //Copy the results into SecureDataBody
    SecureDataBody body;
    body.secure_data.resize(output.size());
    memcpy(body.secure_data.data(),output.data(),output.size());

    //Build Secure DataTag
    SecureDataTag dataTag;
    memcpy(dataTag.common_mac.data(),tag, 16);

    //Check the list of receivers, search for keys and compute session keys as needed
    for(auto rec = receiving_crypto_list.begin(); rec != receiving_crypto_list.end(); ++rec)
    {
        AESGCMGMAC_ParticipantCryptoHandle& remote_participant = AESGCMGMAC_ParticipantCryptoHandle::narrow(**rec);
        if(remote_participant.nil())
        {
            logWarning(SECURITY_CRYPTO,"Invalid CryptoHandle");
            continue;
        }
        if(remote_participant->Participant2ParticipantKeyMaterial.size() == 0)
            continue;

        //Update the key if needed
        if(update_specific_keys){
            //Update triggered!
            remote_participant->session_id = local_participant->session_id;
            remote_participant->SessionKey = compute_sessionkey(remote_participant->Participant2ParticipantKeyMaterial.at(0).master_receiver_specific_key,
                remote_participant->Participant2ParticipantKeyMaterial.at(0).master_salt,
                remote_participant->session_id);
        }
        unsigned char specific_tag[AES_BLOCK_SIZE]; //Container for the Authentication Tag (will become common mac)
        //Obtain MAC using ReceiverSpecificKey and the same Initialization Vector as before
        int actual_size=0, final_size=0;
        EVP_CIPHER_CTX* e_ctx = EVP_CIPHER_CTX_new();
        if( (remote_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES128_GCM)) |
            (remote_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES128_GMAC)))
        {
            EVP_EncryptInit(e_ctx, EVP_aes_128_gcm(), (const unsigned char*)(remote_participant->SessionKey.data()), initialization_vector.data());
        }
        if( (remote_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES256_GCM)) |
            (remote_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES256_GMAC)))
        {
            EVP_EncryptInit(e_ctx, EVP_aes_256_gcm(), (const unsigned char*)(remote_participant->SessionKey.data()), initialization_vector.data());
        }
        EVP_EncryptUpdate(e_ctx, NULL, &actual_size, dataTag.common_mac.data(), 16);
        EVP_EncryptFinal(e_ctx, output.data() + actual_size, &final_size);
        EVP_CIPHER_CTX_ctrl(e_ctx, EVP_CTRL_GCM_GET_TAG, 16, specific_tag);
        output.resize(actual_size+final_size);
        EVP_CIPHER_CTX_free(e_ctx);

        ReceiverSpecificMAC buffer;
        buffer.receiver_mac_key_id = remote_participant->Participant2ParticipantKeyMaterial.at(0).receiver_specific_key_id;
        memcpy(buffer.receiver_mac.data(),specific_tag,16);
        //Push the MAC into the dataTag
        dataTag.receiver_specific_macs.push_back(buffer);
    }

    //Assemble the message
    encoded_rtps_message.clear();

    //Header
    std::vector<uint8_t> serialized_header = serialize_SecureDataHeader(header);
    //Body
    std::vector<uint8_t> serialized_body = serialize_SecureDataBody(body);
    //Tag
    std::vector<uint8_t> serialized_tag = serialize_SecureDataTag(dataTag);

    unsigned char flags = 0x00;
    encoded_rtps_message = assemble_rtps_message(rtps_header, serialized_header, serialized_body, serialized_tag, flags);

    return true;
}

bool AESGCMGMAC_Transform::decode_rtps_message(
                std::vector<uint8_t> &plain_buffer,
                const std::vector<uint8_t> &encoded_buffer,
                const ParticipantCryptoHandle &receiving_crypto,
                ParticipantCryptoHandle &sending_crypto,
                SecurityException &exception){

    AESGCMGMAC_ParticipantCryptoHandle& sending_participant = AESGCMGMAC_ParticipantCryptoHandle::narrow(sending_crypto);

    if(sending_participant.nil())
    {
        logError(SECURITY_CRYPTO, "Invalid sending_crypto handle");
        return false;
    }

    if(sending_participant->RemoteParticipant2ParticipantKeyMaterial.size() == 0)
        return false;

    //Fun reverse order process;
    SecureDataHeader header;
    SecureDataBody body;
    SecureDataTag tag;

    std::vector<uint8_t> serialized_header, serialized_body, serialized_tag;
    std::vector<uint8_t> rtps_header;
    unsigned char flags;

    if(!disassemble_rtps_message(encoded_buffer, rtps_header, serialized_header, serialized_body, serialized_tag, flags))   return false;
    //Header
    header = deserialize_SecureDataHeader(serialized_header);
    //Body
    body = deserialize_SecureDataBody(serialized_body);
    //Tag
    tag = deserialize_SecureDataTag(serialized_tag);
    //Read specific MACs in search for the correct one (verify the authenticity of the message)
    ReceiverSpecificMAC* specific_mac;
    bool mac_found = false;
    for(int j=0; j < tag.receiver_specific_macs.size(); j++){
        //Check if it matches the key we have
        //TODO(Ricardo) Check if its necessary to use a vector.
        if(sending_participant->RemoteParticipant2ParticipantKeyMaterial.at(0).receiver_specific_key_id == tag.receiver_specific_macs.at(j).receiver_mac_key_id){
            mac_found = true;
            specific_mac =  &(tag.receiver_specific_macs.at(j));
            break;
        }
    }

    if(!mac_found){
        logWarning(SECURITY_CRYPTO,"Unable to authenticate the message: message does not target this Participant");
        exception = SecurityException("Message does not contain a suitable specific MAC for the receiving Participant");
        return false;
    }

    uint32_t session_id;
    memcpy(&session_id,header.session_id.data(),4);
    //Sessionkey
    std::array<uint8_t,32> session_key = compute_sessionkey(
            sending_participant->RemoteParticipant2ParticipantKeyMaterial.at(0).master_sender_key,
            sending_participant->RemoteParticipant2ParticipantKeyMaterial.at(0).master_salt,
            session_id);
    //IV
    std::array<uint8_t,12> initialization_vector;
    memcpy(initialization_vector.data(), header.session_id.data(), 4);
    memcpy(initialization_vector.data() + 4, header.initialization_vector_suffix.data(), 8);

    //Auth message - The point is that we cannot verify the authorship of the message with our receiver_specific_key the message could be crafted
    bool auth = false;

    RAND_load_file("/dev/urandom",32);

    EVP_CIPHER_CTX *d_ctx = EVP_CIPHER_CTX_new();
    plain_buffer.clear();
    plain_buffer.resize(encoded_buffer.size());

    int actual_size = 0, final_size = 0;

    //Get ReceiverSpecificSessionKey
    std::array<uint8_t,32> specific_session_key = compute_sessionkey(
                    sending_participant->RemoteParticipant2ParticipantKeyMaterial.at(0).master_receiver_specific_key,
                    sending_participant->RemoteParticipant2ParticipantKeyMaterial.at(0).master_salt,
                    session_id);

    //Verify specific MAC
    if( (sending_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES128_GCM)) |
        (sending_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES128_GMAC)))
    {
      EVP_DecryptInit(d_ctx, EVP_aes_128_gcm(), (const unsigned char *)specific_session_key.data(), initialization_vector.data());
    }
    if( (sending_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES256_GCM)) |
        (sending_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES256_GMAC)))
    {
      EVP_DecryptInit(d_ctx, EVP_aes_256_gcm(), (const unsigned char *)specific_session_key.data(), initialization_vector.data());
    }
    EVP_DecryptUpdate(d_ctx, NULL, &actual_size, tag.common_mac.data(), 16);
    EVP_CIPHER_CTX_ctrl( d_ctx, EVP_CTRL_GCM_SET_TAG,16, specific_mac->receiver_mac.data() );
    auth = EVP_DecryptFinal_ex(d_ctx, plain_buffer.data() + actual_size, &final_size);
    EVP_CIPHER_CTX_free(d_ctx);

    if(!auth){
        logWarning(SECURITY_CRYPTO,"Unable to authenticate the message.");
        return false;
    }

    //Decode message
    RAND_load_file("/dev/urandom",32);

    d_ctx = EVP_CIPHER_CTX_new();
    plain_buffer.clear();

    actual_size = 0;
    final_size = 0;

    if( (sending_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES128_GCM)) |
        (sending_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES128_GMAC)))
    {
      EVP_DecryptInit(d_ctx, EVP_aes_128_gcm(), (const unsigned char *)session_key.data(), initialization_vector.data());
    }
    if( (sending_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES256_GCM)) |
        (sending_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES256_GMAC)))
    {
      EVP_DecryptInit(d_ctx, EVP_aes_256_gcm(), (const unsigned char *)session_key.data(), initialization_vector.data());
    }

    if( (sending_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES256_GCM)) |
        (sending_participant->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES128_GCM)))
    {

        plain_buffer.resize(encoded_buffer.size());
        EVP_DecryptUpdate(d_ctx, plain_buffer.data(), &actual_size, body.secure_data.data(),body.secure_data.size());
        EVP_CIPHER_CTX_ctrl(d_ctx, EVP_CTRL_GCM_SET_TAG,16,tag.common_mac.data());
        EVP_DecryptFinal(d_ctx, plain_buffer.data() + actual_size, &final_size);
        plain_buffer.resize(actual_size + final_size);
    }else{
        plain_buffer.resize(body.secure_data.size());
        memcpy(plain_buffer.data(),body.secure_data.data(),body.secure_data.size());
    }
    EVP_CIPHER_CTX_free(d_ctx);
    return true;
}

bool AESGCMGMAC_Transform::preprocess_secure_submsg(
                DatawriterCryptoHandle **datawriter_crypto,
                DatareaderCryptoHandle **datareader_crypto,
                SecureSubmessageCategory_t &secure_submessage_category,
                const std::vector<uint8_t> encoded_rtps_submessage,
                ParticipantCryptoHandle &receiving_crypto,
                ParticipantCryptoHandle &sending_crypto,
                SecurityException &exception){

    AESGCMGMAC_ParticipantCryptoHandle& remote_participant = AESGCMGMAC_ParticipantCryptoHandle::narrow(sending_crypto);
    if(remote_participant.nil()){
        logWarning(SECURITY_CRYPTO,"Invalid CryptoHandle");
        exception = SecurityException("Not a valid ParticipantCryptoHandle received");
        return false;
    }
    AESGCMGMAC_ParticipantCryptoHandle& local_participant = AESGCMGMAC_ParticipantCryptoHandle::narrow(receiving_crypto);
    if(local_participant.nil()){
        logWarning(SECURITY_CRYPTO,"Invalid CryptoHandle");
        exception = SecurityException("Not a valid ParticipantCryptoHandle received");
        return false;
    }

    SecureDataHeader header;
    SecureDataTag tag;
    std::vector<uint8_t> serialized_header, serialized_body, serialized_tag;
    unsigned char flags;
    if(!disassemble_endpoint_submessage(encoded_rtps_submessage, serialized_header, serialized_body, serialized_tag, flags)){
        logWarning(SECURITY_CRYPTO,"Could not preprocess message, unable to disassemble it");
        return false;
    }

    header = deserialize_SecureDataHeader(serialized_header);
    tag = deserialize_SecureDataTag(serialized_tag);
    //KeyId is present in Header->transform_identifier->transformation_key_id and contains the sender_key_id

    for(std::vector<DatawriterCryptoHandle *>::iterator it = remote_participant->Writers.begin(); it != remote_participant->Writers.end(); ++it){
        AESGCMGMAC_WriterCryptoHandle& writer = AESGCMGMAC_WriterCryptoHandle::narrow(**it);
        if( writer->Writer2ReaderKeyMaterial.at(0).sender_key_id == header.transform_identifier.transformation_key_id){
            secure_submessage_category = DATAWRITER_SUBMESSAGE;
            *datawriter_crypto = *it;
            //We have the remote writer, now lets look for the local datareader
            for(std::vector<DatareaderCryptoHandle *>::iterator itt = local_participant->Readers.begin(); itt != local_participant->Readers.end(); ++itt){
                AESGCMGMAC_ReaderCryptoHandle& reader = AESGCMGMAC_ReaderCryptoHandle::narrow(**itt);
                for(int i=0; i < reader->Reader2WriterKeyMaterial.size(); i++){
                    if(reader->Reader2WriterKeyMaterial.at(i).receiver_specific_key_id == writer->Reader2WriterKeyMaterial.at(0).receiver_specific_key_id){
                        *datareader_crypto = *itt;
                        return true;
                    }
                }   //For each Reader2WriterKeyMaterial in the local datareader
            } //For each datareader present in the local participant
        }
    }

    for(std::vector<DatareaderCryptoHandle *>::iterator it = remote_participant->Readers.begin(); it != remote_participant->Readers.end(); ++it){
        AESGCMGMAC_ReaderCryptoHandle& reader = AESGCMGMAC_ReaderCryptoHandle::narrow(**it);
        if( reader->Reader2WriterKeyMaterial.at(0).sender_key_id == header.transform_identifier.transformation_key_id){
            secure_submessage_category = DATAREADER_SUBMESSAGE;
            *datareader_crypto = *it;
            //We have the remote reader, now lets look for the local datawriter
            for(std::vector<DatawriterCryptoHandle *>::iterator itt = local_participant->Writers.begin(); itt != local_participant->Writers.end(); ++itt){
                AESGCMGMAC_WriterCryptoHandle& writer = AESGCMGMAC_WriterCryptoHandle::narrow(**itt);
                for(int i=0; i < writer->Writer2ReaderKeyMaterial.size(); i++){
                    if(writer->Writer2ReaderKeyMaterial.at(i).receiver_specific_key_id == reader->Writer2ReaderKeyMaterial.at(0).receiver_specific_key_id){
                        *datawriter_crypto = *itt;
                        return true;
                    }
                }   //For each Writer2ReaderKeyMaterial in the local datawriter
            } //For each datawriter present in the local participant
        }
    }
    logWarning(SECURITY_CRYPTO,"Unable to determine the nature of the message");
    return false;
}

bool AESGCMGMAC_Transform::decode_datawriter_submessage(
                std::vector<uint8_t> &plain_rtps_submessage,
                const std::vector<uint8_t> &encoded_rtps_submessage,
                DatareaderCryptoHandle &receiving_datareader_crypto,
                DatawriterCryptoHandle &sending_datawriter_cryupto,
                SecurityException &exception){

    AESGCMGMAC_WriterCryptoHandle& sending_writer = AESGCMGMAC_WriterCryptoHandle::narrow(sending_datawriter_cryupto);

    //Fun reverse order process;
    SecureDataHeader header;
    SecureDataBody body;
    SecureDataTag tag;

    std::vector<uint8_t> serialized_header, serialized_body, serialized_tag;
    unsigned char flags;

    if( !disassemble_endpoint_submessage(encoded_rtps_submessage, serialized_header, serialized_body, serialized_tag, flags) ){
        logWarning(SECURITY_CRYPTO,"Unable to disassemble endpoint submessage");
        return false;
    }
    //Header
    header = deserialize_SecureDataHeader(serialized_header);
    //Body
    body = deserialize_SecureDataBody(serialized_body);
    //Tag
    tag = deserialize_SecureDataTag(serialized_tag);

    //Read specific MACs in search for the correct one (verify the authenticity of the message)
    ReceiverSpecificMAC specific_mac;
    bool mac_found = false;
    for(int j=0; j < tag.receiver_specific_macs.size(); j++){
        //Check if it matches the key we have
        if(sending_writer->Writer2ReaderKeyMaterial.at(0).receiver_specific_key_id == tag.receiver_specific_macs.at(j).receiver_mac_key_id){
            mac_found = true;
            specific_mac =  tag.receiver_specific_macs.at(j);
            break;
        }
    }

    if(!mac_found){
        logWarning(SECURITY_CRYPTO,"Unable to authenticate the message");
        exception = SecurityException("Message does not contain a suitable specific MAC for the receiving Participant");
        return false;
    }

    uint32_t session_id;
    memcpy(&session_id,header.session_id.data(),4);
    //Sessionkey
    std::array<uint8_t,32> session_key = compute_sessionkey(
            sending_writer->Writer2ReaderKeyMaterial.at(0).master_sender_key,
            sending_writer->Writer2ReaderKeyMaterial.at(0).master_salt,
            session_id);
    //IV
    std::array<uint8_t,12> initialization_vector;
    memcpy(initialization_vector.data(), header.session_id.data(), 4);
    memcpy(initialization_vector.data() + 4, header.initialization_vector_suffix.data(), 8);

    //Auth message - The point is that we cannot verify the authorship of the message with our receiver_specific_key the message could be crafted
    bool auth = false;

    RAND_load_file("/dev/urandom",32);

    EVP_CIPHER_CTX *d_ctx = EVP_CIPHER_CTX_new();
    plain_rtps_submessage.clear();
    plain_rtps_submessage.resize(encoded_rtps_submessage.size());

    int actual_size = 0, final_size = 0;

    //Get ReceiverSpecificSessionKey
    std::array<uint8_t,32> specific_session_key = compute_sessionkey(
                    sending_writer->Writer2ReaderKeyMaterial.at(0).master_receiver_specific_key,
                    sending_writer->Writer2ReaderKeyMaterial.at(0).master_salt,
                    session_id);

    //Verify specific MAC
    EVP_DecryptInit(d_ctx, EVP_aes_128_gcm(), (const unsigned char *)specific_session_key.data(), initialization_vector.data());
    EVP_DecryptUpdate(d_ctx, NULL, &actual_size, tag.common_mac.data(), 16);
    EVP_CIPHER_CTX_ctrl( d_ctx, EVP_CTRL_GCM_SET_TAG,16, specific_mac.receiver_mac.data() );
    auth = EVP_DecryptFinal_ex(d_ctx, plain_rtps_submessage.data() + actual_size, &final_size);
    EVP_CIPHER_CTX_free(d_ctx);

    if(!auth){
        logWarning(SECURITY_CRYPTO, "Unable to auth message, it could be coming from a rogue sender");
        return false;
    }

    //Decode message
    RAND_load_file("/dev/urandom",32);

    d_ctx = EVP_CIPHER_CTX_new();
    plain_rtps_submessage.clear();
    plain_rtps_submessage.resize(encoded_rtps_submessage.size());

    actual_size = 0;
    final_size = 0;
    EVP_DecryptInit(d_ctx, EVP_aes_128_gcm(), (const unsigned char *)session_key.data(), initialization_vector.data());
    EVP_DecryptUpdate(d_ctx, plain_rtps_submessage.data(), &actual_size, body.secure_data.data(),body.secure_data.size());
    EVP_CIPHER_CTX_ctrl(d_ctx, EVP_CTRL_GCM_SET_TAG,16,tag.common_mac.data());
    EVP_DecryptFinal(d_ctx, plain_rtps_submessage.data() + actual_size, &final_size);
    EVP_CIPHER_CTX_free(d_ctx);
    plain_rtps_submessage.resize(actual_size + final_size);

    return true;
}

bool AESGCMGMAC_Transform::decode_datareader_submessage(
                std::vector<uint8_t> &plain_rtps_submessage,
                const std::vector<uint8_t> &encoded_rtps_submessage,
                DatawriterCryptoHandle &receiving_datawriter_crypto,
                DatareaderCryptoHandle &sending_datareader_crypto,
                SecurityException &exception){


    AESGCMGMAC_ReaderCryptoHandle& sending_reader = AESGCMGMAC_ReaderCryptoHandle::narrow(sending_datareader_crypto);

    //Fun reverse order process;
    SecureDataHeader header;
    SecureDataBody body;
    SecureDataTag tag;

    std::vector<uint8_t> serialized_header, serialized_body, serialized_tag;
    unsigned char flags;

    if(!disassemble_endpoint_submessage(encoded_rtps_submessage, serialized_header, serialized_body, serialized_tag, flags)){
        logWarning(SECURITY_CRYPTO, "Unable to disassemble endpoint submessage");
        return false;
    }
    //Header
    header = deserialize_SecureDataHeader(serialized_header);
    //Body
    body = deserialize_SecureDataBody(serialized_body);
    //Tag
    tag = deserialize_SecureDataTag(serialized_tag);

    //Read specific MACs in search for the correct one (verify the authenticity of the message)
    ReceiverSpecificMAC specific_mac;
    bool mac_found = false;
    for(int j=0; j < tag.receiver_specific_macs.size(); j++){
        //Check if it matches the key we have
        if(sending_reader->Reader2WriterKeyMaterial.at(0).receiver_specific_key_id == tag.receiver_specific_macs.at(j).receiver_mac_key_id){
            mac_found = true;
            specific_mac = tag.receiver_specific_macs.at(j);
            break;
        }
    }

    if(!mac_found){
        logWarning(SECURITY_CRYPTO, "Unable to auth the message: it is not directed to the recipient that processes it");
        exception = SecurityException("Message does not contain a suitable specific MAC for the receiving Participant");
        return false;
    }
    uint32_t session_id;
    memcpy(&session_id,header.session_id.data(),4);
    //Sessionkey
    std::array<uint8_t,32> session_key = compute_sessionkey(
            sending_reader->Reader2WriterKeyMaterial.at(0).master_sender_key,
            sending_reader->Reader2WriterKeyMaterial.at(0).master_salt,
            session_id);
    //IV
    std::array<uint8_t,12> initialization_vector;
    memcpy(initialization_vector.data(), header.session_id.data(), 4);
    memcpy(initialization_vector.data() + 4, header.initialization_vector_suffix.data(), 8);

    //Auth message - The point is that we cannot verify the authorship of the message with our receiver_specific_key the message could be crafted
    bool auth = false;

    RAND_load_file("/dev/urandom",32);

    EVP_CIPHER_CTX *d_ctx = EVP_CIPHER_CTX_new();
    plain_rtps_submessage.clear();
    plain_rtps_submessage.resize(encoded_rtps_submessage.size());

    int actual_size = 0, final_size = 0;

    //Get ReceiverSpecificSessionKey
    std::array<uint8_t,32> specific_session_key = compute_sessionkey(
                    sending_reader->Reader2WriterKeyMaterial.at(0).master_receiver_specific_key,
                    sending_reader->Reader2WriterKeyMaterial.at(0).master_salt,
                    session_id);

    //Verify specific MAC
    EVP_DecryptInit(d_ctx, EVP_aes_128_gcm(), (const unsigned char *)specific_session_key.data(), initialization_vector.data());
    EVP_DecryptUpdate(d_ctx, NULL, &actual_size, tag.common_mac.data(), 16);
    EVP_CIPHER_CTX_ctrl( d_ctx, EVP_CTRL_GCM_SET_TAG,16, specific_mac.receiver_mac.data() );
    auth = EVP_DecryptFinal_ex(d_ctx, plain_rtps_submessage.data() + actual_size, &final_size);
    EVP_CIPHER_CTX_free(d_ctx);

    if(!auth){
        logWarning(SECURITY_CRYPTO,"Unable to authenticate the message, it may come from a rogue source");
        return false;
    }

    //Decode message
    RAND_load_file("/dev/urandom",32);

    d_ctx = EVP_CIPHER_CTX_new();
    plain_rtps_submessage.clear();
    plain_rtps_submessage.resize(encoded_rtps_submessage.size());

    actual_size = 0;
    final_size = 0;
    EVP_DecryptInit(d_ctx, EVP_aes_128_gcm(), (const unsigned char *)session_key.data(), initialization_vector.data());
    EVP_DecryptUpdate(d_ctx, plain_rtps_submessage.data(), &actual_size, body.secure_data.data(),body.secure_data.size());
    EVP_CIPHER_CTX_ctrl(d_ctx, EVP_CTRL_GCM_SET_TAG,16,tag.common_mac.data());
    EVP_DecryptFinal(d_ctx, plain_rtps_submessage.data() + actual_size, &final_size);
    EVP_CIPHER_CTX_free(d_ctx);
    plain_rtps_submessage.resize(actual_size + final_size);

    return true;


}


bool AESGCMGMAC_Transform::decode_serialized_payload(
                std::vector<uint8_t> &plain_buffer,
                const std::vector<uint8_t> &encoded_buffer,
                const std::vector<uint8_t> &inline_qos,
                DatareaderCryptoHandle &receiving_datareader_crypto,
                DatawriterCryptoHandle &sending_datawriter_crypto,
                SecurityException &exception){

    AESGCMGMAC_WriterCryptoHandle& sending_writer = AESGCMGMAC_WriterCryptoHandle::narrow(sending_datawriter_crypto);
    if(sending_writer.nil()){
        exception = SecurityException("Not a valid sending_writer handle");
        return false;
    }

    //Fun reverse order process
    SecureDataHeader header;
    std::vector<uint8_t> serialized_header;
    SecureDataBody body;
    std::vector<uint8_t> serialized_body;
    SecureDataTag tag;
    std::vector<uint8_t> serialized_tag;

    unsigned char flags = 0x00;

    if( !disassemble_serialized_payload(encoded_buffer, serialized_header, serialized_body, serialized_tag, flags) ){
        logWarning(SECURITY_CRYPTO,"Unable to disassemble the message");
        std::cout << "Disassembly function failure" << std::endl;
        return false;
    }

    //Header
    header = deserialize_SecureDataHeader(serialized_header);
    //Body
    body = deserialize_SecureDataBody(serialized_body);
    //Tag
    tag = deserialize_SecureDataTag(serialized_tag);

    uint32_t session_id;
    memcpy(&session_id,header.session_id.data(),4);

    //Sessionkey
    std::array<uint8_t,32> session_key = compute_sessionkey(
            sending_writer->Writer2ReaderKeyMaterial.at(0).master_sender_key,
            sending_writer->Writer2ReaderKeyMaterial.at(0).master_salt,
            session_id);
    //IV
    std::array<uint8_t,12> initialization_vector;
    memcpy(initialization_vector.data(), header.session_id.data(), 4);
    memcpy(initialization_vector.data() + 4, header.initialization_vector_suffix.data(), 8);


    RAND_load_file("/dev/urandom",32);

    EVP_CIPHER_CTX *d_ctx = EVP_CIPHER_CTX_new();
    int actual_size = 0, final_size = 0;
    plain_buffer.clear();
    plain_buffer.resize(encoded_buffer.size());

    bool return_value;
    if(sending_writer->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES128_GCM)){
      EVP_DecryptInit(d_ctx, EVP_aes_128_gcm(), (const unsigned char *)session_key.data(), initialization_vector.data());
    }
    if(sending_writer->transformation_kind == std::array<uint8_t,4>(CRYPTO_TRANSFORMATION_KIND_AES256_GCM)){
      EVP_DecryptInit(d_ctx, EVP_aes_256_gcm(), (const unsigned char *)session_key.data(), initialization_vector.data());
    }
    EVP_DecryptUpdate(d_ctx, plain_buffer.data(), &actual_size, body.secure_data.data(),body.secure_data.size());
    EVP_CIPHER_CTX_ctrl(d_ctx, EVP_CTRL_GCM_SET_TAG,16,tag.common_mac.data());
    return_value = EVP_DecryptFinal(d_ctx, plain_buffer.data() + actual_size, &final_size);
    EVP_CIPHER_CTX_free(d_ctx);
    plain_buffer.resize(actual_size + final_size);

    return return_value;
}

std::array<uint8_t, 32> AESGCMGMAC_Transform::compute_sessionkey(std::array<uint8_t,32> master_sender_key,std::array<uint8_t,32> master_salt , uint32_t &session_id)
{

    std::array<uint8_t,32> session_key;
    unsigned char *source = (unsigned char*)malloc(32 + 10 + 32 + 4);
    memcpy(source, master_sender_key.data(), 32);
    char seq[] = "SessionKey";
    memcpy(source+32, seq, 10);
    memcpy(source+32+10, master_salt.data(),32);
    memcpy(source+32+10+32, &(session_id),4);

    EVP_Digest(source, 32+10+32+4, (unsigned char*)&(session_key), NULL, EVP_sha256(), NULL);

    free(source);
    return session_key;
}

std::vector<uint8_t> AESGCMGMAC_Transform::serialize_SecureDataHeader(SecureDataHeader &input)
{
    std::vector<uint8_t> buffer;
    int i;

    for(i=0;i < 4; i++) buffer.push_back( input.transform_identifier.transformation_kind.at(i) );
    for(i=0;i < 4; i++) buffer.push_back( input.transform_identifier.transformation_key_id.at(i) );
    for(i=0;i < 4; i++) buffer.push_back( input.session_id.at(i) );
    for(i=0;i < 8; i++) buffer.push_back( input.initialization_vector_suffix.at(i) );

    return buffer;
}

std::vector<uint8_t> AESGCMGMAC_Transform::serialize_SecureDataBody(SecureDataBody &input)
{
    std::vector<uint8_t> buffer;
    int i;

    long body_length = input.secure_data.size();
    for(i=0;i < sizeof(long); i++) buffer.push_back( *( (uint8_t*)&body_length + i) );
    for(i=0;i < body_length; i++) buffer.push_back( input.secure_data.at(i) );

    return buffer;
}

std::vector<uint8_t> AESGCMGMAC_Transform::serialize_SecureDataTag(SecureDataTag &input)
{
    std::vector<uint8_t> buffer;
    int i,j;

    //Common tag
    for(i=0;i < 16; i++) buffer.push_back( input.common_mac.at(i) );
        //Receiver specific macs
    long specific_length = input.receiver_specific_macs.size();
    for(i=0;i < sizeof(long); i++) buffer.push_back( *( (uint8_t*)&specific_length + i ) );
    for(j=0; j< input.receiver_specific_macs.size(); j++){
        for(i=0;i < 4; i++) buffer.push_back( input.receiver_specific_macs.at(j).receiver_mac_key_id.at(i) );
        for(i=0;i < 16; i++) buffer.push_back( input.receiver_specific_macs.at(j).receiver_mac.at(i) );
    }

    return buffer;
}

std::vector<uint8_t> AESGCMGMAC_Transform::assemble_serialized_payload(std::vector<uint8_t> &serialized_header, std::vector<uint8_t> &serialized_body, std::vector<uint8_t> &serialized_tag, unsigned char &flags)
{
    std::vector<uint8_t> buffer;
    int i;

    for(i=0; i < serialized_header.size(); i++) buffer.push_back( serialized_header.at(i) );
    for(i=0; i < serialized_body.size(); i++) buffer.push_back( serialized_body.at(i) );
    for(i=0; i < serialized_tag.size(); i++) buffer.push_back(serialized_tag.at(i) );

    return buffer;
}


std::vector<uint8_t> AESGCMGMAC_Transform::assemble_endpoint_submessage(std::vector<uint8_t> &serialized_header, std::vector<uint8_t> &serialized_body, std::vector<uint8_t> &serialized_tag, unsigned char &flags)
{
    std::vector<uint8_t> buffer;
    int i;
    //TODO(Ricardo) Review bigendianess
    short octets;

    //SEC_PREFIX
    buffer.push_back(SEC_PREFIX);
    //Flags
    flags &= 0xFE; //Force LSB to zero
    buffer.push_back(flags);
    //Octets2NextSubMessageHeader
    octets = serialized_header.size() + serialized_body.size() + 2 + 2 + serialized_tag.size();
    uint8_t octets_c[2] = { 0, 0 };
    memcpy(octets_c, &octets, 2);
    buffer.push_back( octets_c[1] );
    buffer.push_back( octets_c[0] );

    //SecureDataHeader
    for(i=0; i < serialized_header.size(); i++) buffer.push_back( serialized_header.at(i) );
    //Payload
    for(i=0; i < serialized_body.size(); i++)   buffer.push_back( serialized_body.at(i) );
    //SEC_POSTFIX
    buffer.push_back(SEC_POSTFIX);
    //Flags
    buffer.push_back(flags);
    //Octets2NextSubMessageHeader
    octets = serialized_tag.size();
    memcpy(octets_c, &octets, 2);
    buffer.push_back( octets_c[1] );
    buffer.push_back( octets_c[0] );

    //SecureDataTag
    for(int i=0; i < serialized_tag.size(); i++)    buffer.push_back( serialized_tag.at(i) );

    return buffer;
}

std::vector<uint8_t> AESGCMGMAC_Transform::assemble_rtps_message(std::vector<uint8_t> &rtps_header, std::vector<uint8_t> &serialized_header, std::vector<uint8_t> &serialized_body, std::vector<uint8_t> &serialized_tag, unsigned char &flags)
{
    std::vector<uint8_t> buffer;
    int i;
    short octets;

    //Unaltered Header
    for(i=0; i < rtps_header.size(); i++)   buffer.push_back( rtps_header.at(i) );
    //SRTPS_PREFIX
    buffer.push_back(SRTPS_PREFIX);
    //Flags
    flags &= 0xFE; //Enforce LSB to zero
    buffer.push_back(flags);
    //Octects2NextSugMs
    octets = serialized_header.size() + serialized_body.size() + 2 + 2 + serialized_tag.size();
    uint8_t octets_c[2] = { 0, 0 };
    memcpy(octets_c, &octets, 2);
    buffer.push_back( octets_c[1] );
    buffer.push_back( octets_c[0] );
    //Header
    for(i=0; i < serialized_header.size(); i++) buffer.push_back( serialized_header.at(i) );
    //Payload
    for(i=0; i < serialized_body.size(); i++)   buffer.push_back( serialized_body.at(i) );
    //SRTPS_POSTFIX
    buffer.push_back(SRTPS_POSTFIX);
    //Flags
    buffer.push_back(flags);
    //Octets2Nextheader
    octets = serialized_tag.size();
    memcpy(octets_c, &octets, 2);
    buffer.push_back( octets_c[1] );
    buffer.push_back( octets_c[0] );
    //Tag
    for(int i=0; i < serialized_tag.size(); i++)    buffer.push_back( serialized_tag.at(i) );

    return buffer;
}

SecureDataHeader AESGCMGMAC_Transform::deserialize_SecureDataHeader(std::vector<uint8_t> &input){

    SecureDataHeader header;
    int i;

    for(i=0;i<4;i++) header.transform_identifier.transformation_kind.at(i) = ( input.at( i ) );
    for(i=0;i<4;i++) header.transform_identifier.transformation_key_id.at(i) = ( input.at( i+4 ) );
    for(i=0;i<4;i++) header.session_id.at(i) = ( input.at( i+8 ) );
    for(i=0;i<8;i++) header.initialization_vector_suffix.at(i) = ( input.at( i+12 ) );

    return header;
}

SecureDataBody AESGCMGMAC_Transform::deserialize_SecureDataBody(std::vector<uint8_t> &input){

    SecureDataBody body;

    long body_length = 0;
    memcpy(&body_length, input.data(), sizeof(long));
    for(int i=0;i < body_length; i++) body.secure_data.push_back( input.at( i + sizeof(long) ) );

    return body;
}

SecureDataTag AESGCMGMAC_Transform::deserialize_SecureDataTag(std::vector<uint8_t> &input){

    SecureDataTag tag;

    //Tag
        //common_mac
    for(int i=0;i < 16; i++) tag.common_mac.at(i) = ( input.at( i ) );
        //receiver_specific_mac
    long spec_length = 0;
    memcpy(&spec_length, input.data()+16, sizeof(long));
    //Read specific MACs in search for the correct one (verify the authenticity of the message)
    ReceiverSpecificMAC specific_mac;
    for(int j=0; j < spec_length; j++){
        memcpy( &(specific_mac.receiver_mac_key_id),
                input.data() + 16 + sizeof(long) + j*(20),
                4 );
        memcpy( specific_mac.receiver_mac.data(),
                input.data() + 16 + sizeof(long) + 4,
                16 );
        tag.receiver_specific_macs.push_back(specific_mac);
    }

    return tag;
}

bool AESGCMGMAC_Transform::disassemble_serialized_payload(const std::vector<uint8_t> &input, std::vector<uint8_t> &serialized_header, std::vector<uint8_t> &serialized_body, std::vector<uint8_t> &serialized_tag, unsigned char &flags)
{

    int i;

    serialized_header.clear();
    for(i=0; i < 20; i++) serialized_header.push_back( input.at(i) );

    serialized_body.clear();
    long body_length = 0;
    memcpy(&body_length, input.data() + 20, sizeof(long));
    for(i=0; i < ( sizeof(long) + body_length ); i++) serialized_body.push_back( input.at(i + 20) );

    serialized_tag.clear();
    for(i=0; i < ( input.size() - 20 - body_length - sizeof(long) ); i++) serialized_tag.push_back(input.at(i + 20 + sizeof(long) + body_length) );

    return true;
}

bool AESGCMGMAC_Transform::disassemble_endpoint_submessage(const std::vector<uint8_t> &input, std::vector<uint8_t> &serialized_header, std::vector<uint8_t> &serialized_body, std::vector<uint8_t> &serialized_tag, unsigned char &flags)
{

    short offset = 0;
    int i;

    //SRTPS_PREFIX
    if( input.at(offset) != SEC_PREFIX ){
        std::cout << "Not a valid prefix" << std::endl;
        return false;
    }
    offset += 1;
    //Flags are ignored for the time being
    offset +=1;
    //Octects2NextSugMsg
    uint8_t octets_c[2] = { 0, 0 };
    octets_c[1] = input.at(offset);
    offset += 1;
    octets_c[0] = input.at(offset);
    offset += 1;
    short safecheck;
    memcpy(&safecheck, octets_c, 2);
    if( (input.size() - offset) != safecheck){
        std::cout << "Not a valid length" << std::endl;
        return false;
    }
    //Header
    serialized_header.clear();
    for(i=0; i < 20; i++) serialized_header.push_back( input.at(offset + i) );
    offset += 20;
    //Payload
    serialized_body.clear();
    long body_length = 0;
    memcpy(&body_length, input.data() + offset, sizeof(long));
    for(i=0; i < ( sizeof(long) + body_length ); i++) serialized_body.push_back( input.at(i + offset) );
    offset += sizeof(long) + body_length;
    //SRTPS_POSTFIX
    if( input.at(offset) != SEC_POSTFIX ){
        std::cout << "Not a valid length" << std::endl;
        return false;
    }
    offset += 1;
    //Flags
    offset += 1;
    //Octets2Nextheader
    octets_c[1] = input.at(offset);
    offset += 1;
    octets_c[0] = input.at(offset);
    offset += 1;
    memcpy(&safecheck, octets_c, 2);
    if( (input.size() - offset) != safecheck)   return false;
    //Tag
    serialized_tag.clear();
    for(i=0; i < ( input.size() - offset ); i++) serialized_tag.push_back(input.at(i + offset) );

    return true;
}

bool AESGCMGMAC_Transform::disassemble_rtps_message(const std::vector<uint8_t> &input, std::vector<uint8_t> &rtps_header, std::vector<uint8_t> &serialized_header, std::vector<uint8_t> &serialized_body, std::vector<uint8_t> &serialized_tag, unsigned char &flags)
{

    short offset = 0;
    int i;

    //Unaltered Header
    rtps_header.clear();
    for(i=0; i < RTPS_HEADER_SIZE; i++)   rtps_header.push_back( input.at(i + offset) );
    offset += RTPS_HEADER_SIZE;
    //SRTPS_PREFIX
    if( input.at(offset) != SRTPS_PREFIX ) return false;
    offset += 1;
    //Flags are ignored for the time being
    offset +=1;
    //Octects2NextSugMsg
    uint8_t octets_c[2] = { 0, 0 };
    octets_c[1] = input.at(offset);
    offset += 1;
    octets_c[0] = input.at(offset);
    offset += 1;
    short safecheck;
    memcpy(&safecheck, octets_c, 2);
    if( (input.size() - offset) != safecheck){
        return false;
    }
    //Header
    serialized_header.clear();
    for(i=0; i < 20; i++) serialized_header.push_back( input.at(i + offset) );
    offset += 20;
    //Payload
    serialized_body.clear();
    long body_length = 0;
    memcpy(&body_length, input.data() + offset, sizeof(long));
    for(i=0; i < ( sizeof(long) + body_length ); i++) serialized_body.push_back( input.at(i + offset) );
    offset += sizeof(long) + body_length;
    //SRTPS_POSTFIX
    if( input.at(offset) != SRTPS_POSTFIX ) return false;
    offset += 1;
    //Flags are ignored for the time being
    offset += 1;
    //Octets2Nextheader
    octets_c[1] = input.at(offset);
    offset += 1;
    octets_c[0] = input.at(offset);
    offset += 1;
    memcpy(&safecheck, octets_c, 2);
    if( (input.size() - offset) != safecheck){
        return false;
    }
    //Tag
    serialized_tag.clear();
    for(i=0; i < safecheck; i++) serialized_tag.push_back(input.at(i + offset) );

    return true;
}

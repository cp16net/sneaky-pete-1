#include "pch.hpp"
#include "RedisException.h"


namespace nova { namespace redis {

RedisException::RedisException(RedisException::Code code) throw()
: code(code)
{
}

RedisException::~RedisException() throw() {
}

const char * RedisException::what() const throw() {
    switch(code) {
        case CHANGE_PASSWORD_ERROR:
            return "Error changing passwords.";
        case COMMAND_ERROR:
            return "Error sending command.";
        case CONNECTION_ERROR:
            return "Couldn't create connection to Redis!";
        case COULD_NOT_START:
            return "Could not stop Redis!";
        case COULD_NOT_STOP:
            return "Could not stop Redis!";
        case INVALID_BACKUP_TARGET:
            return "Instance cannot be used for backup command.";
        case LOCAL_CONF_READ_ERROR:
            return "Error reading Redis local.conf.";
        case LOCAL_CONF_WRITE_ERROR:
            return "Error writing Redis local conf.";
        case MISSING_ROOT_PASSWORD:
            return "Missing root password argument in prepare call!";
        case PREPARE_ERROR:
            return "An error occured while preparing Redis.";
        case REPLY_ERROR:
            return "Error in Redis reply.";
        case RESPONSE_TIMEOUT:
            return "Timeout receiving response from Redis.";
        case UNABLE_TO_WRITE_CONFIG:
            return "Unable to write new config.";
        case UNKNOWN_INFO_VALUE:
            return "Unknown info key value.";
        default:
            return "An error occurred.";
    }
}

} }  // end namespace

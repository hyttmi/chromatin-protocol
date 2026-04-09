// Asio SSL separate compilation unit.
// Required because the project uses ASIO_SEPARATE_COMPILATION.
// The asio.cmake wrapper compiles asio/impl/src.hpp but NOT ssl/impl/src.hpp.
#include <asio/ssl/impl/src.hpp>

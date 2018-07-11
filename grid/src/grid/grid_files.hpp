#ifndef GRID_GRIDFILES_HPP
#define GRID_GRIDFILES_HPP

#include <iostream>
#include <vector>

#include "grid.hpp"

// TODO(alex): These files do not account for the platform endianness. This
// should be fixed if we plan on using "grid" on non little endian machines.

// This binary file contains the data at the beginning followed by
// a header with the parameters that we need to interpret the data properly.
// The data is stored in little endian format and using 64 bits of precision for
// double floating points values.
namespace Grid::Files::Dat {

// This structure is saved after the parameters as a way to extract and
// interpret the footer data with the grid parameters. For now, saving the
// spec_version in case we need to do revisions to the format in the future.
struct Parameters {
    char spec_version;
    char footer_length;
};

// Load an entire file from the binary stream into the destination vector and
// the parameters structure. Returns the success or failure of the operation.
bool read(std::istream &stream, std::vector<double> *destination,
          Grid::Parameters *parameters);

// Write the entire source vector and parameters struct into the destination
// binary stream. Returns the success or failure of the operation.
bool write(std::ostream &stream, const std::vector<double> &source,
           const Grid::Parameters &parameters);

// Loads a specific range from the given stream. Both the destination and
// parameters arguments will be properly modified. Returns the success or
// failure of the operation.
bool read_range(std::istream &stream, const Grid::Bounds &bounds,
                std::vector<double> *destination, Grid::Parameters *parameters);

// Write the specific range and derivated parameters into the file stream.
// Returns the success or failure of the operation.
bool write_range(std::ostream &stream, const Grid::Bounds &bounds,
                 const std::vector<double> &source,
                 const Grid::Parameters &parameters);

// Load the parameters from the footer of the binary stream. Returns the success
// or failure of the operation.
bool read_parameters(std::istream &stream, Grid::Parameters *parameters);
bool write_parameters(std::ostream &stream, const Grid::Parameters &parameters);

}  // namespace Grid::Files::Dat

// The binary file containing the dump of all the points present in the original
// file. This allows for quickly reading the raw data without the need of
// parsing or decoding the original file. The values are stored in little endian
// format using 64 bits of precision for floating point values.
//
// The serialization is very simple. The first 8 bytes store a uint64_t with the
// number of points contained in the file, followed by a vector of Grid::Point
// objects.
namespace Grid::Files::Rawdump {
// TODO(alex): Should we write the parameters into the rawdump? This would allow
// us to load the rawdump independently of the datfile or parameters.

// Write the entire source vector in the given binary stream.
bool write(std::ostream &stream, const std::vector<Grid::Point> &points);

// Load the entire source vector from the given binary stream.
bool read(std::istream &stream, std::vector<Grid::Point> &points);

}  // namespace Grid::Files::Rawdump

namespace Grid::Files::Csv {
// TODO(alex): Read/write grid as a CSV file that can be opened in Excel, R,
// Python or other statistical software. Should this be implemented?
}  // namespace Grid::Files::Csv

#endif /* GRID_GRIDFILES_HPP */

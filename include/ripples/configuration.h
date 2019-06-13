//===------------------------------------------------------------*- C++ -*-===//
//
//             Ripples: A C++ Library for Influence Maximization
//                  Marco Minutoli <marco.minutoli@pnnl.gov>
//                   Pacific Northwest National Laboratory
//
//===----------------------------------------------------------------------===//
//
// Copyright 2018 Battelle Memorial Institute
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors
// may be used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//===----------------------------------------------------------------------===//

#ifndef RIPPLES_CONFIGURATION_H
#define RIPPLES_CONFIGURATION_H

#include <string>

#include "CLI11/CLI11.hpp"

namespace ripples {

//! \brief Descriptor for the input Graph
//!
//! The GraphInputConfiguration stores the command-line input describing the
//! input graphs.
struct GraphInputConfiguration {
  std::string IFileName{""};  //!< The input file name
  bool weighted{false};       //!< is Graph weighted?
  bool undirected{false};     //!< is Graph undirected?
  bool reload{false};         //!< are we reloading a binary dump?

  //! \brief Add command line options for the input grah.
  //!
  //! \param app The command-line parser object.
  void addCmdOptions(CLI::App &app) {
    app.add_option("-i,--input-graph", IFileName,
                   "The input file with the edge-list.")
        ->group("Input Options")
        ->required();
    app.add_flag("--reload-binary", reload, "Reload a graph from binary input")
        ->group("Input Options");
    app.add_flag("-u,--undirected", undirected, "The input graph is undirected")
        ->group("Input Options");
    app.add_flag("-w,--weighted", weighted, "The input graph is weighted")
        ->group("Input Options");
  }
};

//! \brief Descriptor for the output of the tool.
//!
//! The OutputConfiguration stores the command-line input describing the output
//! of the tool.
struct OutputConfiguration {
  std::string OutputFile{"output.json"};  //!< The file name of the log

  //! \brief Add command line options for the output of the tool.
  //!
  //! \param app The command-line parser object.
  void addCmdOptions(CLI::App &app) {
    app.add_option("-o,--output", OutputFile, "The file name of the log.")
        ->group("Output Options");
  }
};

//! \brief Command-line configuration descriptor
//!
//! The class describes the input coming from the command-line interface.
//!
//! \tparam AlgorithmConfiguration Configuration descriptor of the algorithm to
//! run.
//! \tparam OutputConfiguration Configuration descriptor for the output of the
//! tool.
//! \tparam InputConfiguration Configuration descriptor for the input of the
//! tool.
template <typename AlgorithmConfiguration,
          typename OutputConfiguration = OutputConfiguration,
          typename InputConfiguration = GraphInputConfiguration>
struct ToolConfiguration : public InputConfiguration,
                           public AlgorithmConfiguration,
                           public OutputConfiguration {
  //! \brief Parse command-line options.
  //!
  //! \param argc Length of the command-line options vector.
  //! \param argv Command-line options vector.
  void ParseCmdOptions(int argc, char **argv) {
    CLI::App app;

    InputConfiguration::addCmdOptions(app);
    AlgorithmConfiguration::addCmdOptions(app);
    OutputConfiguration::addCmdOptions(app);

    try {
      app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
      exit(app.exit(e));
    }
  }
};

};  // namespace ripples

#endif  // RIPPLES_CONFIGURATION_H
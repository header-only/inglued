#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <vector>
#include <regex>
#include <boost/range/algorithm/find.hpp>
#include <boost/format.hpp>
#include <json.hpp>
#include <boost/algorithm/string.hpp>
#include <future>
#include <boost/process.hpp>
#include <boost/filesystem.hpp>
#include <boost/process/system.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/filesystem/path.hpp>

#include <inglued/dep.hpp>
#include <inglued/generate_cmakelists.hpp>
#include <inglued/generate_install_doc.hpp>


namespace inglued {

  using fmt = boost::format;
  namespace bp = boost::process;
  namespace fs = boost::filesystem;


  //! Reads the list of deps in the from the given  file, and if it exits it's accompanying .transitive.
  inline map_deps_t read_deps(const std::string& path) {
    map_deps_t deps;
    
    auto to_dep = [](nlohmann::json::iterator& it) {
      dep d;
      d.git_uri = it.key();
      d.ref = it.value()["@"].get<std::string>();
      
      if (!it.value()["-I"].is_null()) {
        d.include_path = it.value()["-I"].get<std::string>();
      }

      return d;
    };   

    { // Direct dependencies
      std::ifstream ifs(path.data(), std::ios::in );

      nlohmann::json j;
      ifs >> j;
      for (nlohmann::json::iterator it = j.begin(); it != j.end(); ++it) {
        auto d = to_dep(it);
        deps[d.git_uri] = d;
      }
    }

    auto transitive_path = path + ".transitive";
    if (fs::exists(transitive_path)) { // Transitive dependencies
      std::ifstream ifs(transitive_path.data(), std::ios::in );

      nlohmann::json j;
      ifs >> j;
      for (nlohmann::json::iterator it = j.begin(); it != j.end(); ++it) {
        auto d = to_dep(it);
        d.transitive = true;
        deps[d.git_uri] = d;
      }
    }

    return deps;
  }

  //! Reads the list of deps in the deps.inglued json file.
  inline void write_transitive_deps(const std::string& path, const map_deps_t& deps) {
    auto to_json = [](auto d) {
      nlohmann::json dep_j;
      dep_j["@"] = d.second.ref;

      if (d.second.include_path != "") {
        dep_j["-I"] = d.second.include_path;
      }

      return dep_j;
    };

    auto transitive_path = (path + ".transitive");
    auto commit_transitive_manifest = [&transitive_path]() {
      bp::system(bp::shell, "git", "add", transitive_path);
      bp::system(bp::shell, "git", "commit", transitive_path, 
        "-m", "\"#inglued : transitive dependencies manifest.\"");
    };

    { // Transitive dependencies

      nlohmann::json j;
      for (auto& dep_entry : deps ) {
        if (dep_entry.second.transitive) {
          j[dep_entry.first] = to_json(dep_entry);
        }
      }

      if (j.size() > 0) {
        std::fstream ofs(transitive_path.data(), std::ios::in | std::ios::out | std::ios::trunc );
        ofs << j.dump(2);
        ofs.close();
        commit_transitive_manifest();
      }
    }


  }


  //! Run git to install the dep if not already present
  inline void check_and_clone(const dep& d) {

    std::cout << "\t fetching " << d.git_uri << "..." << std::flush;

    std::future<std::string> buffer;

    boost::asio::io_service ios;
    bp::child c(bp::search_path("git"), std::string("subtree")
      , (fs::exists(fs::path("deps") / d.get_name()) ? "pull" : "add")
      , std::string("--prefix=") + "deps/" + d.get_name()
      , d.get_uri()
      , d.ref
      , "--squash"
      , ios
      , (bp::std_out & bp::std_err) > buffer);

    ios.run();
    c.wait();

    if (c.exit_code() != 0) {
      throw std::runtime_error(
        std::string("Cannot add the dependency, your working tree must not have "
        "changes to already commited files. Output is : \n ") + buffer.get());
    }

    std::cout << " ok." << std::endl;

  }

  //! Delves in each dependencies at 1 depth level and lookup for further includesive deps. If some pull them up here.
  inline void hikeup_deep_deps(map_deps_t& deps) {

    auto scan_inglued_dep = [](const fs::path& glue_file) {
      auto deep_deps = read_deps((const std::string &)glue_file.native());

      std::for_each(deep_deps.begin(), deep_deps.end(),
        [](auto& p) { p.second.transitive  = true;});

      return deep_deps;
    };

    //TODO: Deal with #inglued lib which is used by non-inglued one.
    std::vector<std::string> already_scanned;
    auto scan_non_inglued_dep = [&already_scanned](dep d, const auto& self) -> map_deps_t {
      using boost::range::find;
      map_deps_t deep_deps;

      if (find(already_scanned, d.git_uri) == already_scanned.end()) { 
        deep_deps = inglued::adapter::boostorg(d);
        already_scanned.push_back(d.git_uri);

        for (auto deep_d : deep_deps) {

          if (find(already_scanned, deep_d.second.git_uri) == already_scanned.end()) { 
            inglued::check_and_clone(deep_d.second); 
          }

          auto deeper_deps = self(deep_d.second, self);
          deep_deps.insert(deeper_deps.begin(), deeper_deps.end());
        }

      }
        
        return deep_deps;
    };



    for (auto& d : deps) {

      auto glue_file = fs::path("deps") / d.second.get_name() / GLUE_PATH;
      if (fs::exists(glue_file)) {
        auto deep_deps = scan_inglued_dep(glue_file);
        deps.insert(deep_deps.begin(), deep_deps.end());

      } else {
        auto deep_deps = scan_non_inglued_dep(d.second, scan_non_inglued_dep);
        deps.insert(deep_deps.begin(), deep_deps.end());

      }
    }

    std::cout << "Fetching and glueing in your git repository transitive dependencies:" << std::endl;
    // Now that we have hiked up the deps of our deps, clone and add them again at our level.
    // We have to duplicate the files because windows-git-clone symlinks impossible. Sad.
    for (auto& d : deps) {
      if (d.second.transitive) { inglued::check_and_clone(d.second); } 
    }

    // Write down the transitive deps
    std::cout << "Declaring the transitive dependencies in " << GLUE_PATH << ".transitive" << std::endl;
    write_transitive_deps(GLUE_PATH, deps);
    std::cout << std::endl;
  }

 


  //TODO: Inform user of what happenned : present him a tree and it's git changes. Tell him how to revert : glue unseal ?
  //TODO: Generate CONSUME.md with all compilers include path generated to explain how to use.
  

}

inline void show_help() {
  std::cout << "#inglued <> : The header-only dependency manager for library authors.\n";
  std::cout << "\n";

  std::cout << "Simply make a deps/inglued file listing your header-only github dependencies, \n"
               "and inglued will commit them to your repository !\n"
               "\n"
               "Actually the best way for your users to consume libraries : just clone / download\n"
               "and all is in the package!\n"
               "\n"
               "No git-submodule but good old git-subtree !\n"
                
               "Commands : \n"
               "\t inglued seal : Put all dependencies listed in `deps/inglued` in your git repository.\n"
               "\t inglued cmake <org> <project> <project_srcs> : Generates 2 stuffs.\n"
               "\t\t - CMakeLists.txt to let your library be cmake find_packaged\n"
               "\t\t - INSTALL.md to help your users\n"
               "\n"
               "\t inglued cmaketpl : Create a CMakeLists.txt.tpl that you can tweak if you need custom additions\n"
               << std::endl;
}

int main(int argc, const char* argv[]) {
  
  using boost::algorithm::ends_with;
  using boost::algorithm::starts_with;

  std::string cmd{(argc > 1) ? argv[1] : "help"};
  if ( (argc <= 1) || ( (argc > 1) && (ends_with(cmd,"help")) ) ) {
    show_help();

  } else if ( cmd == "seal" ) {
    auto deps = inglued::read_deps(inglued::GLUE_PATH);
    
    std::cout << "Fetching and glueing in your git repository direct dependencies:" << std::endl;
    for (auto& d : deps) {
      if (!d.second.transitive) { inglued::check_and_clone(d.second); }
    }
    std::cout << std::endl;

    std::cout << "Fetching and glueing in your git repository transitive dependencies:" << std::endl;
    hikeup_deep_deps(deps);

  } else if ( (argc > 4) && (cmd == "cmake") ) {
    auto deps = inglued::read_deps(inglued::GLUE_PATH);
    std::cout << "Generating CMakeLists.txt to allow IDE support, test to be build and sysroot installation." << std::endl;
    //TODO: argv[2] and argv[3] could be taken from default origin remote if github.com
    inglued::generate_cmakelists(argv[2], argv[3], argv[4], deps);
    inglued::generate_install_doc(argv[2], argv[3], argv[4], deps);

  } else if ( (argc < 4) && (cmd == "cmake") ) {
    show_help();
    std::cout << "Not enough arguments to cmake command !" << std::endl;
  
  } else if (cmd == "cmaketpl") {
    inglued::generate_cmakelists_tpl();

  }

  return 0;
}

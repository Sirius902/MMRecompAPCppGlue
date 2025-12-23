#include <cstdio>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <vector>

#include "apcpp-solo-gen.h"

// _DEBUG causes Python to link the debug binary, which isn't present in normal installs.
#undef _DEBUG
#include <Python.h>

// Converts a path to a UTF-8 encoded std::string by going through a std::u8string.
std::string path_to_string_utf8(const std::filesystem::path& path) {
    std::u8string path_u8string = path.u8string();
    std::string to_escape{ reinterpret_cast<const char*>(path_u8string.c_str()), path_u8string.size() };

    std::string ret{};
    ret.reserve(to_escape.size());
    for (char c : to_escape) {
        // Escape backslashes
        if (c == '\\') {
            ret += '\\';
        }
        ret += c;
    }
    return ret;
}

extern "C" {
    extern unsigned char python_zip[];
    extern unsigned char minipelago_zip[];
    extern int python_zip_len;
    extern int minipelago_zip_len;
}

struct ZipEntry {
    unsigned char* data;
    int size;
    std::filesystem::path name;
};

// This allows for multiple zips to be copied, but only one ended up being used.
// There may be a use case for multiple zips in the future.
ZipEntry zips[] = {
    { .data = minipelago_zip, .size = minipelago_zip_len, .name = "minipelago.zip" },
};

// Writes a zip to disk.
void write_zip(const std::filesystem::path& zip_path, const ZipEntry& zip) {
    std::ofstream out{ zip_path, std::ios::binary };
    out.write(reinterpret_cast<const char*>(zip.data), zip.size);
}

// Checks if the zip on disk (if it exists) is identical to the one that will be written.
bool compare_zip(const std::filesystem::path& zip_path, const ZipEntry& zip) {
    // If the file doesn't exist then it's not equivalent.
    if (!std::filesystem::exists(zip_path)) {
        return false;
    }

    std::ifstream old_zip{ zip_path, std::ios::binary };

    // If the file couldn't be opened, treat it as not equivalent.
    if (!old_zip.good()) {
        return false;
    }

    // If the file is a different size then it's not equivalent.
    old_zip.seekg(0, std::ios::end);
    size_t old_zip_size = old_zip.tellg();
    if (old_zip_size != zip.size) {
        return false;
    }

    // Read the file's contents.
    old_zip.seekg(0, std::ios::beg);
    std::vector<char> data;
    data.resize(old_zip_size);
    old_zip.read(data.data(), data.size());

    // Compare the two files.
    return std::memcmp(data.data(), zip.data, old_zip_size) == 0;
}

// Writes any zips that aren't identical to the contents in memory. 
void update_zips(const std::filesystem::path& zips_dir) {
    for (size_t i = 0; i < std::size(zips); i++) {
        std::filesystem::path zip_path = zips_dir / zips[i].name;
        if (!compare_zip(zip_path, zips[i])) {
            printf("Zip %s differed\n", zips[i].name.string().c_str());
            write_zip(zip_path, zips[i]);
        }
        else {
            printf("Zip %s was the same\n", zips[i].name.string().c_str());
        }
    }
}

bool sologen::generate(const std::filesystem::path& yaml_dir, const std::filesystem::path& output_dir) {
    // Update the zips the first time a generation runs.
    static bool updated_zips = false;
    if (!updated_zips) {
        update_zips(output_dir);
        updated_zips = true;
    }

    PyStatus status;
    
    PyPreConfig preconfig;
    PyPreConfig_InitPythonConfig(&preconfig);
    Py_PreInitialize(&preconfig);

    PyConfig config;
    PyConfig_InitPythonConfig(&config);  // Use isolated config if you don't want to inherit env

    PyConfig_SetBytesString(&config, &config.program_name, "minipelago");
    
    wchar_t* zip_path = nullptr;
    status = PyConfig_SetBytesString(&config, &zip_path, path_to_string_utf8(output_dir / zips[0].name).c_str());

    config.module_search_paths_set = 1;
    PyWideStringList_Append(&config.module_search_paths, zip_path);
    PyMem_RawFree(zip_path);

    Py_InitializeFromConfig(&config);
    PyRun_SimpleString("import sys");

    // remove last path entry and add the zip file based on the current dynamic library path
    PyRun_SimpleString("sys.path.pop()");
    auto mod_zip_path = std::string("sys.path.insert(0, '") + path_to_string_utf8(output_dir / zips[0].name) + "')";
    PyRun_SimpleString(mod_zip_path.c_str());

    // debug print sys.path
    PyRun_SimpleString("print(sys.path)");

    // operate in yaml dir
    auto chdir_cmd = std::string("import os; os.chdir('") + path_to_string_utf8(yaml_dir) + "')";
    PyRun_SimpleString(chdir_cmd.c_str());

    // Create a Python list to simulate sys.argv
    std::vector<std::string> args = {
        "MMGenerate.py",  // argv[0] is typically the script name
        "--player_files_path", path_to_string_utf8(yaml_dir),
        "--outputpath", path_to_string_utf8(output_dir)
    };
  
    PyObject* py_argv = PyList_New(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        PyList_SetItem(py_argv, i, PyUnicode_FromString(args[i].c_str()));
    }

    // Set sys.argv
    PySys_SetObject("argv", py_argv);

    PyObject* globals = PyDict_New();
    PyObject* locals = PyDict_New();
    PyObject* result = PyRun_String(
        "from MMGenerate import main as generate\n"
        "generate()",
        Py_file_input, globals, locals
    );

    bool success;
    if (result) {
        success = true;
        Py_DECREF(result);
    }
    else {
        success = false;
        // Exception occurred
        if (PyErr_Occurred()) {
            PyErr_Print();  // Print to stderr
        }
    }

    Py_DECREF(globals);
    Py_DECREF(locals);

    Py_DECREF(py_argv);

    // finalize python
    Py_Finalize();

    return success;
}

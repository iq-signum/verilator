// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Options parsing
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2025 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "V3PchAstMT.h"

#include "V3Options.h"

#include "V3Error.h"
#include "V3File.h"
#include "V3Global.h"
#include "V3Mutex.h"
#include "V3OptionParser.h"
#include "V3Os.h"
#include "V3PreShell.h"
#include "V3String.h"

// clang-format off
#include <sys/types.h>
#include <sys/stat.h>
#ifndef _WIN32
# include <sys/utsname.h>
#endif
#include <algorithm>
#include <cctype>
#ifdef _MSC_VER
# include <filesystem> // C++17
# define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#else
# include <dirent.h>
#endif
#include <fcntl.h>
#include <list>
#include <map>
#include <memory>
#include <thread>
#include <set>
#include <string>

#include "config_rev.h"

#if defined(_WIN32) || defined(__MINGW32__)
# include <io.h>  // open, close
#endif
// clang-format on

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
// V3 Internal state

class V3OptionsImp final {
public:
    // TYPES
    using DirMap = std::map<const string, std::set<std::string>>;  // Directory listing

    // STATE
    std::list<string> m_lineArgs;  // List of command line argument encountered
    std::list<string> m_allArgs;  // List of every argument encountered
    std::list<string> m_incDirUsers;  // Include directories (ordered)
    std::set<string> m_incDirUserSet;  // Include directories (for removing duplicates)
    std::list<string> m_incDirFallbacks;  // Include directories (ordered)
    std::set<string> m_incDirFallbackSet;  // Include directories (for removing duplicates)
    std::map<const string, V3LangCode> m_langExts;  // Language extension map
    std::list<string> m_libExtVs;  // Library extensions (ordered)
    std::set<string> m_libExtVSet;  // Library extensions (for removing duplicates)
    DirMap m_dirMap;  // Directory listing

    // ACCESSOR METHODS
    void addIncDirUser(const string& incdir) {
        const string& dir = V3Os::filenameCleanup(incdir);
        const auto itFoundPair = m_incDirUserSet.insert(dir);
        if (itFoundPair.second) {
            // cppcheck-suppress stlFindInsert  // cppcheck 1.90 bug
            m_incDirUsers.push_back(dir);
            m_incDirFallbacks.remove(dir);  // User has priority over Fallback
            m_incDirFallbackSet.erase(dir);  // User has priority over Fallback
        }
    }
    void addIncDirFallback(const string& incdir) {
        const string& dir = V3Os::filenameCleanup(incdir);
        if (m_incDirUserSet.find(dir)
            == m_incDirUserSet.end()) {  // User has priority over Fallback
            const auto itFoundPair = m_incDirFallbackSet.insert(dir);
            if (itFoundPair.second) m_incDirFallbacks.push_back(dir);
        }
    }
    void addLangExt(const string& langext, const V3LangCode& lc) {
        // New language extension replaces any pre-existing one.
        string addext = langext;
        if (addext[0] == '.') addext = addext.substr(1);
        (void)m_langExts.erase(addext);
        m_langExts[addext] = lc;
    }

    void addLibExtV(const string& libext) {
        const auto itFoundPair = m_libExtVSet.insert(libext);
        if (itFoundPair.second) m_libExtVs.push_back(libext);
    }
    V3OptionsImp() = default;
    ~V3OptionsImp() = default;
};

//######################################################################
// V3LangCode class functions

V3LangCode::V3LangCode(const char* textp) {
    // Return code for given string, or ERROR, which is a bad code
    for (int codei = V3LangCode::L_ERROR; codei < V3LangCode::_ENUM_END; ++codei) {
        const V3LangCode code{codei};
        if (0 == VL_STRCASECMP(textp, code.ascii())) {
            m_e = code;
            return;
        }
    }
    m_e = V3LangCode::L_ERROR;
}

//######################################################################
// VTimescale class functions

VTimescale::VTimescale(const string& value, bool& badr)
    : m_e{VTimescale::NONE} {
    badr = true;
    const string spaceless = VString::removeWhitespace(value);
    for (int i = TS_100S; i < _ENUM_END; ++i) {
        const VTimescale ts{i};
        if (spaceless == ts.ascii()) {
            badr = false;
            m_e = ts.m_e;
            break;
        }
    }
}

//######################################################################
// V3HierarchicalBlockOption class functions

// Parse "--hierarchical-block orig_name,mangled_name,param0_name,param0_value,... " option.
// The format of value is as same as -G option. (can be string literal surrounded by ")
V3HierarchicalBlockOption::V3HierarchicalBlockOption(const string& opts) {
    V3StringList vals;
    bool inStr = false;
    string cur;
    static const string hierBlock("--hierarchical-block");
    FileLine cmdfl(FileLine::commandLineFilename());
    // Split by ','. If ',' appears between "", that is not a separator.
    for (string::const_iterator it = opts.begin(); it != opts.end();) {
        if (inStr) {
            if (*it == '\\') {
                ++it;
                if (it == opts.end()) {
                    cmdfl.v3error(hierBlock + " must not end with \\");
                    break;
                }
                if (*it != '"' && *it != '\\') {
                    cmdfl.v3error(hierBlock + " does not allow '" + *it + "' after \\");
                    break;
                }
                cur.push_back(*it);
                ++it;
            } else if (*it == '"') {  // end of string
                cur.push_back(*it);
                vals.push_back(cur);
                cur.clear();
                ++it;
                if (it != opts.end()) {
                    if (*it != ',') {
                        cmdfl.v3error(hierBlock + " expects ',', but '" + *it + "' is passed");
                        break;
                    }
                    ++it;
                    if (it == opts.end()) {
                        cmdfl.v3error(hierBlock + " must not end with ','");
                        break;
                    }
                    inStr = *it == '"';
                    cur.push_back(*it);
                    ++it;
                }
            } else {
                cur.push_back(*it);
                ++it;
            }
        } else {
            if (*it == '"') {
                cmdfl.v3error(hierBlock + " does not allow '\"' in the middle of literal");
                break;
            }
            if (*it == ',') {  // end of this parameter
                vals.push_back(cur);
                cur.clear();
                ++it;
                if (it == opts.end()) {
                    cmdfl.v3error(hierBlock + " must not end with ','");
                    break;
                }
                inStr = *it == '"';
            }
            cur.push_back(*it);
            ++it;
        }
    }
    if (!cur.empty()) vals.push_back(cur);
    if (vals.size() >= 2) {
        if (vals.size() % 2) {
            cmdfl.v3error(hierBlock + " requires the number of entries to be even");
        }
        m_origName = vals[0];
        m_mangledName = vals[1];
    } else {
        cmdfl.v3error(hierBlock + " requires at least two comma-separated values");
    }
    for (size_t i = 2; i + 1 < vals.size(); i += 2) {
        const bool inserted = m_parameters.emplace(vals[i], vals[i + 1]).second;
        if (!inserted) {
            cmdfl.v3error("Module name '" + vals[i] + "' is duplicated in " + hierBlock);
        }
    }
}

//######################################################################
// V3Options class functions

void VTimescale::parseSlashed(FileLine* fl, const char* textp, VTimescale& unitr,
                              VTimescale& precr, bool allowEmpty) {
    // Parse `timescale of <number><units> / <number><units>
    unitr = VTimescale::NONE;
    precr = VTimescale::NONE;

    const char* cp = textp;
    for (; std::isspace(*cp); ++cp) {}
    const char* const unitp = cp;
    for (; *cp && *cp != '/'; ++cp) {}
    const string unitStr(unitp, cp - unitp);
    for (; std::isspace(*cp); ++cp) {}
    string precStr;
    if (*cp == '/') {
        ++cp;
        for (; std::isspace(*cp); ++cp) {}
        const char* const precp = cp;
        for (; *cp && *cp != '/'; ++cp) {}
        precStr = string(precp, cp - precp);
    }
    for (; std::isspace(*cp); ++cp) {}
    if (*cp) {
        fl->v3error("`timescale syntax error: '" << textp << "'");
        return;
    }

    bool unitbad = false;
    const VTimescale unit{unitStr, unitbad /*ref*/};
    if (unitbad && !(unitStr.empty() && allowEmpty)) {
        fl->v3error("`timescale timeunit syntax error: '" << unitStr << "'");
        return;
    }
    unitr = unit;

    if (!precStr.empty()) {
        VTimescale prec{VTimescale::NONE};
        bool precbad;
        prec = VTimescale{precStr, precbad /*ref*/};
        if (precbad) {
            fl->v3error("`timescale timeprecision syntax error: '" << precStr << "'");
            return;
        }
        if (!unit.isNone() && !prec.isNone() && unit < prec) {
            fl->v3error("`timescale timeunit '"
                        << unitStr << "' must be greater than or equal to timeprecision '"
                        << precStr << "'");
            return;
        }
        precr = prec;
    }
}

//######################################################################
// V3Options class functions

void V3Options::addIncDirUser(const string& incdir) { m_impp->addIncDirUser(incdir); }
void V3Options::addIncDirFallback(const string& incdir) { m_impp->addIncDirFallback(incdir); }
void V3Options::addLangExt(const string& langext, const V3LangCode& lc) {
    m_impp->addLangExt(langext, lc);
}
void V3Options::addLibExtV(const string& libext) { m_impp->addLibExtV(libext); }
void V3Options::addDefine(const string& defline, bool allowPlus) VL_MT_DISABLED {
    // Split +define+foo=value into the appropriate parts and parse
    // Optional + says to allow multiple defines on the line
    // + is not quotable, as other simulators do not allow that
    string left = defline;
    while (left != "") {
        string def = left;
        string::size_type pos;
        if (allowPlus && ((pos = left.find('+')) != string::npos)) {
            left = left.substr(pos + 1);
            def.erase(pos);
        } else {
            left = "";
        }
        string value;
        if ((pos = def.find('=')) != string::npos) {
            value = def.substr(pos + 1);
            def.erase(pos);
        }
        V3PreShell::defineCmdLine(def, value);
    }
}
void V3Options::addParameter(const string& paramline, bool allowPlus) {
    // Split +define+foo=value into the appropriate parts and parse
    // Optional + says to allow multiple defines on the line
    // + is not quotable, as other simulators do not allow that
    string left = paramline;
    while (left != "") {
        string param = left;
        string::size_type pos;
        if (allowPlus && ((pos = left.find('+')) != string::npos)) {
            left = left.substr(pos + 1);
            param.erase(pos);
        } else {
            left = "";
        }
        string value;
        if ((pos = param.find('=')) != string::npos) {
            value = param.substr(pos + 1);
            param.erase(pos);
        }
        UINFO(4, "Add parameter" << param << "=" << value);
        (void)m_parameters.erase(param);
        m_parameters[param] = value;
    }
}

bool V3Options::hasParameter(const string& name) {
    return m_parameters.find(name) != m_parameters.end();
}

string V3Options::parameter(const string& name) {
    string value = m_parameters.find(name)->second;
    m_parameters.erase(m_parameters.find(name));
    return value;
}

void V3Options::checkParameters() {
    if (!m_parameters.empty()) {
        std::stringstream msg;
        msg << "Parameters from the command line were not found in the design:";
        for (const auto& i : m_parameters) msg << " " << i.first;
        v3error(msg.str());
    }
}

void V3Options::addCppFile(const string& filename) { m_cppFiles.insert(filename); }
void V3Options::addCFlags(const string& filename) { m_cFlags.push_back(filename); }
void V3Options::addCompilerIncludes(const string& filename) {
    m_compilerIncludes.insert(filename);
}
void V3Options::addLdLibs(const string& filename) { m_ldLibs.push_back(filename); }
void V3Options::addMakeFlags(const string& filename) { m_makeFlags.push_back(filename); }
void V3Options::addFuture(const string& flag) { m_futures.insert(flag); }
void V3Options::addFuture0(const string& flag) { m_future0s.insert(flag); }
void V3Options::addFuture1(const string& flag) { m_future1s.insert(flag); }
bool V3Options::isFuture(const string& flag) const {
    return m_futures.find(flag) != m_futures.end();
}
bool V3Options::isFuture0(const string& flag) const {
    return m_future0s.find(flag) != m_future0s.end();
}
bool V3Options::isFuture1(const string& flag) const {
    return m_future1s.find(flag) != m_future1s.end();
}
bool V3Options::isLibraryFile(const string& filename, const string& libname) const {
    return m_libraryFiles.find({filename, libname}) != m_libraryFiles.end();
}
void V3Options::addLibraryFile(const string& filename, const string& libname) {
    m_libraryFiles.insert({filename, libname});
}
bool V3Options::isClocker(const string& signame) const {
    return m_clockers.find(signame) != m_clockers.end();
}
void V3Options::addClocker(const string& signame) { m_clockers.insert(signame); }
bool V3Options::isNoClocker(const string& signame) const {
    return m_noClockers.find(signame) != m_noClockers.end();
}
void V3Options::addNoClocker(const string& signame) { m_noClockers.insert(signame); }
void V3Options::addVFile(const string& filename, const string& libname) {
    // We use a list for v files, because it's legal to have includes
    // in a specific order and multiple of them.
    m_vFiles.push_back({filename, libname});
}
void V3Options::addVltFile(const string& filename, const string& libname) {
    m_vltFiles.insert({filename, libname});
}
void V3Options::addForceInc(const string& filename) { m_forceIncs.push_back(filename); }

void V3Options::addLineArg(const string& arg) { m_impp->m_lineArgs.push_back(arg); }

void V3Options::addArg(const string& arg) { m_impp->m_allArgs.push_back(arg); }

string V3Options::allArgsString() const VL_MT_SAFE {
    string result;
    for (const string& i : m_impp->m_allArgs) {
        if (result != "") result += " ";
        result += i;
    }
    return result;
}

// Delete some options for Verilation of the hierarchical blocks.
string V3Options::allArgsStringForHierBlock(bool forTop) const {
    std::set<string> vFiles;
    for (const auto& vFile : m_vFiles) vFiles.insert(vFile.filename());
    string out;
    bool stripArg = false;
    bool stripArgIfNum = false;
    for (const string& arg : m_impp->m_lineArgs) {
        if (stripArg) {
            stripArg = false;
            continue;
        }
        if (stripArgIfNum) {
            stripArgIfNum = false;
            if (isdigit(arg[0])) continue;
        }
        int skip = 0;
        if (arg.length() >= 2 && arg[0] == '-' && arg[1] == '-') {
            skip = 2;
        } else if (arg.length() >= 1 && arg[0] == '-') {
            skip = 1;
        }
        if (skip > 0) {  // arg is an option
            const string opt = arg.substr(skip);  // Remove '-' in the beginning
            const int numStrip = stripOptionsForChildRun(opt, forTop);
            if (numStrip) {
                UASSERT(0 <= numStrip && numStrip <= 3, "should be one of 0, 1, 2, 3");
                if (numStrip == 2) stripArg = true;
                if (numStrip == 3) stripArgIfNum = true;
                continue;
            }
        } else {  // Not an option
            if (vFiles.find(arg) != vFiles.end()  // Remove HDL
                || m_cppFiles.find(arg) != m_cppFiles.end()) {  // Remove C++
                continue;
            }
        }
        if (out != "") out += " ";
        // Don't use opt here because '-' is removed in arg
        // Use double quote because arg may contain whitespaces
        out += '"' + VString::quoteAny(arg, '"', '\\') + '"';
    }
    return out;
}

void V3Options::ccSet() {  // --cc
    m_outFormatOk = true;
    m_systemC = false;
}

void V3Options::decorations(FileLine* fl, const string& arg) {  // --decorations
    if (arg == "none") {
        m_decoration = false;
        m_decorationNodes = false;
    } else if (arg == "node") {
        m_decoration = true;
        m_decorationNodes = true;
    } else if (arg == "medium") {
        m_decoration = true;
        m_decorationNodes = false;
    } else {
        fl->v3error("Unknown setting for --decorations: '"
                    << arg << "'\n"
                    << fl->warnMore() << "... Suggest 'none', 'medium', or 'node'");
    }
}

//######################################################################
// File searching

bool V3Options::fileStatNormal(const string& filename) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct stat sstat;  // Stat information
    const int err = stat(filename.c_str(), &sstat);
    if (err != 0) return false;
    if (S_ISDIR(sstat.st_mode)) return false;
    return true;
}

string V3Options::fileExists(const string& filename) {
    // Surprisingly, for VCS and other simulators, this process
    // is quite slow; presumably because of re-reading each directory
    // many times.  So we read a whole dir at once and cache it

    const string dir = V3Os::filenameDir(filename);
    const string basename = V3Os::filenameNonDir(filename);

    auto diriter = m_impp->m_dirMap.find(dir);
    if (diriter == m_impp->m_dirMap.end()) {
        // Read the listing
        m_impp->m_dirMap.emplace(dir, std::set<string>());
        diriter = m_impp->m_dirMap.find(dir);

        std::set<string>* const setp = &(diriter->second);

#ifdef _MSC_VER
        try {
            for (const auto& dirEntry : std::filesystem::directory_iterator(dir.c_str()))
                setp->insert(dirEntry.path().filename().string());
        } catch (std::filesystem::filesystem_error const& ex) {
            (void)ex;
            return "";
        }
#else
        if (DIR* const dirp = opendir(dir.c_str())) {
            while (struct dirent* direntp = readdir(dirp)) setp->insert(direntp->d_name);
            closedir(dirp);
        }
#endif
    }
    // Find it
    const std::set<string>* const filesetp = &(diriter->second);
    const auto fileiter = filesetp->find(basename);
    if (fileiter == filesetp->end()) return "";  // Not found
    // Check if it is a directory, ignore if so
    const string filenameOut = V3Os::filenameJoin(dir, basename);
    if (!fileStatNormal(filenameOut)) return "";  // Directory
    return filenameOut;
}

string V3Options::filePathCheckOneDir(const string& modname, const string& dirname) {
    for (const string& i : m_impp->m_libExtVs) {
        const string fn = V3Os::filenameJoin(dirname, modname + i);
        const string exists = fileExists(fn);
        if (exists != "") return exists;
    }
    return "";
}

// Checks if a option needs to be stripped for child run of hierarchical Verilation.
// 0: Keep the option including its argument
// 1: Delete the option which has no argument
// 2: Delete the option and its argument
// 3: Delete the option and its argument if it is a number
int V3Options::stripOptionsForChildRun(const string& opt, bool forTop) {
    if (opt == "j") return 3;
    if (opt == "Mdir" || opt == "clk" || opt == "lib-create" || opt == "f" || opt == "F"
        || opt == "v" || opt == "l2-name" || opt == "mod-prefix" || opt == "prefix"
        || opt == "protect-lib" || opt == "protect-key" || opt == "threads"
        || opt == "top-module") {
        return 2;
    }
    if (opt == "build" || (!forTop && (opt == "cc" || opt == "exe" || opt == "sc"))
        || opt == "hierarchical" || (opt.length() > 2 && opt.substr(0, 2) == "G=")) {
        return 1;
    }
    return 0;
}

void V3Options::validateIdentifier(FileLine* fl, const string& arg, const string& opt) {
    if (!VString::isIdentifier(arg)) {
        fl->v3error(opt << " argument must be a legal C++ identifier: '" << arg << "'");
    }
}

string V3Options::filePath(FileLine* fl, const string& modname, const string& lastpath,
                           const string& errmsg) {  // Error prefix or "" to suppress error
    // Find a filename to read the specified module name,
    // using the incdir and libext's.
    // Return "" if not found.
    const string filename = V3Os::filenameCleanup(VName::dehash(modname));
    if (!V3Os::filenameIsRel(filename)) {
        // filename is an absolute path, so can find getStdPackagePath()/getStdWaiverPath()
        const string exists = filePathCheckOneDir(filename, "");
        if (exists != "") return exists;
    }
    for (const string& dir : m_impp->m_incDirUsers) {
        const string exists = filePathCheckOneDir(filename, dir);
        if (exists != "") return exists;
    }
    for (const string& dir : m_impp->m_incDirFallbacks) {
        const string exists = filePathCheckOneDir(filename, dir);
        if (exists != "") return exists;
    }

    if (m_relativeIncludes) {
        const string exists = filePathCheckOneDir(filename, lastpath);
        if (exists != "") return V3Os::filenameRealPath(exists);
    }

    // Warn and return not found
    if (errmsg != "") {
        fl->v3error(errmsg << "'"s << filename << "'\n"s << fl->warnContextPrimary()
                           << V3Error::warnAdditionalInfo() << filePathLookedMsg(fl, filename));
    }
    return "";
}

string V3Options::filePathLookedMsg(FileLine* fl, const string& modname) {
    static bool shown_notfound_msg = false;
    std::ostringstream ss;
    if (modname.find("__Vhsh") != string::npos) {
        ss << V3Error::warnMore() << "... Note: Name is longer than 127 characters; automatic"
           << " file lookup may have failed due to OS filename length limits.\n";
        ss << V3Error::warnMore() << "... Suggest putting filename with this module/package"
           << " onto command line instead.\n";
    } else if (!shown_notfound_msg) {
        shown_notfound_msg = true;
        if (m_impp->m_incDirUsers.empty()) {
            ss << V3Error::warnMore()
               << "... This may be because there's no search path specified with -I<dir>.\n";
        }
        ss << V3Error::warnMore() << "... Looked in:\n";
        for (const string& dir : m_impp->m_incDirUsers) {
            for (const string& ext : m_impp->m_libExtVs) {
                const string fn = V3Os::filenameJoin(dir, modname + ext);
                ss << V3Error::warnMore() << "     " << fn << "\n";
            }
        }
        for (const string& dir : m_impp->m_incDirFallbacks) {
            for (const string& ext : m_impp->m_libExtVs) {
                const string fn = V3Os::filenameJoin(dir, modname + ext);
                ss << V3Error::warnMore() << "     " << fn << "\n";
            }
        }
    }
    return ss.str();
}

//! Determine what language is associated with a filename

//! If we recognize the extension, use its language, otherwise, use the
//! default language.
V3LangCode V3Options::fileLanguage(const string& filename) {
    string ext = V3Os::filenameNonDir(filename);
    string::size_type pos;
    if (filename == V3Options::getStdPackagePath() || filename == V3Options::getStdWaiverPath()) {
        return V3LangCode::mostRecent();
    } else if ((pos = ext.rfind('.')) != string::npos) {
        ext.erase(0, pos + 1);
        const auto it = m_impp->m_langExts.find(ext);
        if (it != m_impp->m_langExts.end()) return it->second;
    }
    return m_defaultLanguage;
}

//######################################################################
// Environment

string V3Options::getenvBuiltins(const string& var) {
    // If update below, also update V3Options::showVersion()
    if (var == "MAKE") {
        return getenvMAKE();
    } else if (var == "PERL") {
        return getenvPERL();
    } else if (var == "PYTHON3") {
        return getenvPYTHON3();
    } else if (var == "SYSTEMC") {
        return getenvSYSTEMC();
    } else if (var == "SYSTEMC_ARCH") {
        return getenvSYSTEMC_ARCH();
    } else if (var == "SYSTEMC_INCLUDE") {
        return getenvSYSTEMC_INCLUDE();
    } else if (var == "SYSTEMC_LIBDIR") {
        return getenvSYSTEMC_LIBDIR();
    } else if (var == "VERILATOR_ROOT") {
        return getenvVERILATOR_ROOT();
    } else {
        return V3Os::getenvStr(var, "");
    }
}

#ifdef __FreeBSD__
string V3Options::getenvMAKE() { return V3Os::getenvStr("MAKE", "gmake"); }
#else
string V3Options::getenvMAKE() { return V3Os::getenvStr("MAKE", "make"); }
#endif

string V3Options::getenvMAKEFLAGS() {  //
    return V3Os::getenvStr("MAKEFLAGS", "");
}

string V3Options::getenvPERL() {  //
    return V3Os::filenameCleanup(V3Os::getenvStr("PERL", "perl"));
}

string V3Options::getenvPYTHON3() {  //
    return V3Os::filenameCleanup(V3Os::getenvStr("PYTHON3", "python3"));
}

string V3Options::getenvSYSTEMC() {
    string var = V3Os::getenvStr("SYSTEMC", "");
    // Treat compiled-in DEFENV string literals as C-strings to enable
    // binary patching for relocatable installs (e.g. conda)
    string defenv = string{DEFENV_SYSTEMC}.c_str();
    if (var == "" && defenv != "") {
        var = defenv;
        V3Os::setenvStr("SYSTEMC", var, "Hardcoded at build time");
    }
    return V3Os::filenameCleanup(var);
}

string V3Options::getenvSYSTEMC_ARCH() {
    string var = V3Os::getenvStr("SYSTEMC_ARCH", "");
    // Treat compiled-in DEFENV string literals as C-strings to enable
    // binary patching for relocatable installs (e.g. conda)
    string defenv = string{DEFENV_SYSTEMC_ARCH}.c_str();
    if (var == "" && defenv != "") {
        var = defenv;
        V3Os::setenvStr("SYSTEMC_ARCH", var, "Hardcoded at build time");
    }
    if (var == "") {
#if defined(__MINGW32__)
        // Hardcoded with MINGW current version. Would like a better way.
        const string sysname = "MINGW32_NT-5.0";
        var = "mingw32";
#elif defined(_WIN32)
        const string sysname = "WIN32";
        var = "win32";
#else
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct utsname uts;
        uname(&uts);
        const string sysname = VString::downcase(uts.sysname);  // aka  'uname -s'
        if (VL_UNCOVERABLE(VString::wildmatch(sysname.c_str(), "*solaris*"))) {
            var = "gccsparcOS5";  // LCOV_EXCL_LINE
        } else if (VL_UNCOVERABLE(VString::wildmatch(sysname.c_str(), "*cygwin*"))) {
            var = "cygwin";  // LCOV_EXCL_LINE
        } else {
            var = "linux";
        }
#endif
        V3Os::setenvStr("SYSTEMC_ARCH", var, "From sysname '" + sysname + "'");
    }
    return var;
}

string V3Options::getenvSYSTEMC_INCLUDE() {
    string var = V3Os::getenvStr("SYSTEMC_INCLUDE", "");
    // Treat compiled-in DEFENV string literals as C-strings to enable
    // binary patching for relocatable installs (e.g. conda)
    string defenv = string{DEFENV_SYSTEMC_INCLUDE}.c_str();
    if (var == "" && defenv != "") {
        var = defenv;
        V3Os::setenvStr("SYSTEMC_INCLUDE", var, "Hardcoded at build time");
    }
    if (var == "") {
        const string sc = getenvSYSTEMC();
        if (sc != "") var = V3Os::filenameJoin(sc, "include");
    }
    return V3Os::filenameCleanup(var);
}

string V3Options::getenvSYSTEMC_LIBDIR() {
    string var = V3Os::getenvStr("SYSTEMC_LIBDIR", "");
    // Treat compiled-in DEFENV string literals as C-strings to enable
    // binary patching for relocatable installs (e.g. conda)
    string defenv = string{DEFENV_SYSTEMC_LIBDIR}.c_str();
    if (var == "" && defenv != "") {
        var = defenv;
        V3Os::setenvStr("SYSTEMC_LIBDIR", var, "Hardcoded at build time");
    }
    if (var == "") {
        const string sc = getenvSYSTEMC();
        const string arch = getenvSYSTEMC_ARCH();
        if (sc != "" && arch != "") var = V3Os::filenameJoin(sc, "lib-" + arch);
    }
    return V3Os::filenameCleanup(var);
}

string V3Options::getenvVERILATOR_ROOT() {
    string var = V3Os::getenvStr("VERILATOR_ROOT", "");
    // Treat compiled-in DEFENV string literals as C-strings to enable
    // binary patching for relocatable installs (e.g. conda)
    string defenv = string{DEFENV_VERILATOR_ROOT}.c_str();
    if (var == "" && defenv != "") {
        var = defenv;
        V3Os::setenvStr("VERILATOR_ROOT", var, "Hardcoded at build time");
    }
    if (var == "") v3fatal("$VERILATOR_ROOT needs to be in environment\n");
    return V3Os::filenameCleanup(var);
}

string V3Options::getenvVERILATOR_SOLVER() {
    string var = V3Os::getenvStr("VERILATOR_SOLVER", "");
    // Treat compiled-in DEFENV string literals as C-strings to enable
    // binary patching for relocatable installs (e.g. conda)
    string defenv = string{DEFENV_VERILATOR_SOLVER}.c_str();
    if (var == "" && defenv != "") {
        var = defenv;
        V3Os::setenvStr("VERILATOR_SOLVER", var, "Hardcoded at build time");
    }
    return var;
}

string V3Options::getStdPackagePath() {
    return V3Os::filenameJoin(getenvVERILATOR_ROOT(), "include", "verilated_std.sv");
}
string V3Options::getStdWaiverPath() {
    return V3Os::filenameJoin(getenvVERILATOR_ROOT(), "include", "verilated_std_waiver.vlt");
}

string V3Options::getSupported(const string& var) {
    // If update below, also update V3Options::showVersion()
    if (var == "COROUTINES" && coroutineSupport()) {
        return "1";
    } else if (var == "SYSTEMC" && systemCFound()) {
        return "1";
    } else {
        return "";
    }
}

bool V3Options::systemCSystemWide() {
#ifdef HAVE_SYSTEMC
    return true;
#else
    return false;
#endif
}

bool V3Options::systemCFound() {
    return (systemCSystemWide()
            || (!getenvSYSTEMC_INCLUDE().empty() && !getenvSYSTEMC_LIBDIR().empty()));
}

bool V3Options::coroutineSupport() {
#ifdef HAVE_COROUTINES
    return true;
#else
    return false;
#endif
}

//######################################################################
// V3 Options notification methods

void V3Options::notify() VL_MT_DISABLED {
    // Notify that all arguments have been passed and final modification can be made.
    FileLine* const cmdfl = new FileLine{FileLine::commandLineFilename()};

    if (!outFormatOk() && v3Global.opt.main()) ccSet();  // --main implies --cc if not provided
    if (!outFormatOk() && !dpiHdrOnly() && !lintOnly() && !preprocOnly() && !serializeOnly()) {
        v3fatal("verilator: Need --binary, --cc, --sc, --dpi-hdr-only, --lint-only, "
                "--xml-only, --json-only or --E option");
    }

    if (m_build && (m_gmake || m_cmake || m_makeJson)) {
        cmdfl->v3error("--make cannot be used together with --build. Suggest see manual");
    }

    // m_build, m_preprocOnly, m_dpiHdrOnly, m_lintOnly, m_jsonOnly and m_xmlOnly are mutually
    // exclusive
    std::vector<std::string> backendFlags;
    if (m_build) {
        if (m_binary)
            backendFlags.push_back("--binary");
        else
            backendFlags.push_back("--build");
    }
    if (m_preprocOnly) backendFlags.push_back("-E");
    if (m_dpiHdrOnly) backendFlags.push_back("--dpi-hdr-only");
    if (m_lintOnly) backendFlags.push_back("--lint-only");
    if (m_xmlOnly) backendFlags.push_back("--xml-only");
    if (m_jsonOnly) backendFlags.push_back("--json-only");
    if (backendFlags.size() > 1) {
        std::string backendFlagsString = backendFlags.front();
        for (size_t i = 1; i < backendFlags.size(); i++) {
            backendFlagsString += ", " + backendFlags[i];
        }
        v3error("The following cannot be used together: " + backendFlagsString
                + ". Suggest see manual");
    }

    if (m_exe && !v3Global.opt.libCreate().empty()) {
        cmdfl->v3error("--exe cannot be used together with --lib-create. Suggest see manual");
    }

    // Make sure at least one make system is enabled
    if (!m_gmake && !m_cmake && !m_makeJson) m_gmake = true;

    if (m_hierarchical && (m_hierChild || !m_hierBlocks.empty())) {
        cmdfl->v3error(
            "--hierarchical must not be set with --hierarchical-child or --hierarchical-block");
    }
    if (m_hierChild) {
        if (m_hierBlocks.empty()) {
            cmdfl->v3error("--hierarchical-block must be set when --hierarchical-child is set");
        }
        m_main = false;
    }

    if (protectIds()) {
        if (allPublic()) {
            // We always call protect() on names, we don't check if public or not
            // Hence any external references wouldn't be able to find the refed public object.
            cmdfl->v3warn(E_UNSUPPORTED, "Unsupported: Using --protect-ids with --public\n"  //
                                             + cmdfl->warnMore() + "... Suggest remove --public.");
        }
        if (trace()) {
            cmdfl->v3warn(INSECURE,
                          "Using --protect-ids with --trace may expose private design details\n"
                              + cmdfl->warnMore() + "... Suggest remove --trace.");
        }
        if (vpi()) {
            cmdfl->v3warn(INSECURE,
                          "Using --protect-ids with --vpi may expose private design details\n"
                              + cmdfl->warnMore() + "... Suggest remove --vpi.");
        }
    }

    // Default some options if not turned on or off
    if (v3Global.opt.skipIdentical().isDefault()) {
        v3Global.opt.m_skipIdentical.setTrueOrFalse(  //
            !v3Global.opt.dpiHdrOnly()  //
            && !v3Global.opt.lintOnly()  //
            && !v3Global.opt.preprocOnly()  //
            && !v3Global.opt.serializeOnly());
    }
    if (v3Global.opt.makeDepend().isDefault()) {
        v3Global.opt.m_makeDepend.setTrueOrFalse(  //
            !v3Global.opt.dpiHdrOnly()  //
            && !v3Global.opt.lintOnly()  //
            && !v3Global.opt.preprocOnly()  //
            && !v3Global.opt.serializeOnly());
    }

    if (trace()) {
        // With --trace-vcd, --trace-threads is ignored
        if (traceFormat().vcd()) m_traceThreads = 1;
    }

    UASSERT(!(useTraceParallel() && useTraceOffload()),
            "Cannot use both parallel and offloaded tracing");

    // Default split limits if not specified
    if (m_outputSplitCFuncs < 0) m_outputSplitCFuncs = m_outputSplit;
    if (m_outputSplitCTrace < 0) m_outputSplitCTrace = m_outputSplit;

    if (v3Global.opt.main() && v3Global.opt.systemC()) {
        cmdfl->v3warn(E_UNSUPPORTED,
                      "--main not usable with SystemC. Suggest see examples for sc_main().");
    }

    if (coverage() && savable()) {
        cmdfl->v3error("Unsupported: --coverage and --savable not supported together");
    }
    if (v3Global.opt.timing().isSetTrue() && savable()) {
        cmdfl->v3error("Unsupported: --timing and --savable not supported together");
    }

    // --dump-tree-dot will turn on tree dumping.
    if (!m_dumpLevel.count("tree") && m_dumpLevel.count("tree-dot")) {
        m_dumpLevel["tree"] = m_dumpLevel["tree-dot"];
    }

    // Sanity check of expected configuration
    UASSERT(threads() >= 1, "'threads()' must return a value >= 1");
    if (m_outputGroups == -1) m_outputGroups = (m_buildJobs != -1) ? m_buildJobs : 0;
    if (m_buildJobs == -1) m_buildJobs = 1;
    if (m_verilateJobs == -1) m_verilateJobs = 1;

    // Preprocessor defines based on options used
    if (timing().isSetTrue()) V3PreShell::defineCmdLine("VERILATOR_TIMING", "1");

    // === Leave last
    // Mark options as available
    m_available = true;
}

//######################################################################
// V3 Options accessors

string V3Options::version() VL_PURE {
    string ver = PACKAGE_STRING;
    ver += " rev " + cvtToStr(DTVERSION_rev);
    return ver;
}

string V3Options::protectKeyDefaulted() VL_MT_SAFE {
    static V3Mutex mutex;
    const V3LockGuard lock{mutex};
    if (m_protectKey.empty()) {
        // Create a key with a human-readable symbol-like name.
        // This conversion drops ~2 bits of entropy out of 256, shouldn't matter.
        VHashSha256 digest{V3Os::trueRandom(32)};
        m_protectKey = "VL-KEY-" + digest.digestSymbol();
    }
    return m_protectKey;
}

void V3Options::throwSigsegv() {  // LCOV_EXCL_START
#if !(defined(VL_CPPCHECK) || defined(__clang_analyzer__))
    // clang-format off
    *static_cast<volatile char*>(nullptr) = 0;  // Intentional core dump, ignore warnings here
    // clang-format on
#endif
}  // LCOV_EXCL_STOP

VTimescale V3Options::timeComputePrec(const VTimescale& flag) const {
    if (!timeOverridePrec().isNone()) {
        return timeOverridePrec();
    } else if (flag.isNone()) {
        return timeDefaultPrec();
    } else {
        return flag;
    }
}

VTimescale V3Options::timeComputeUnit(const VTimescale& flag) const {
    if (!timeOverrideUnit().isNone()) {
        return timeOverrideUnit();
    } else if (flag.isNone()) {
        return timeDefaultUnit();
    } else {
        return flag;
    }
}

int V3Options::unrollCountAdjusted(const VOptionBool& full, bool generate, bool simulate) {
    int count = unrollCount();
    // std::max to avoid rollover if unrollCount is e.g. std::numeric_limits<int>::max()
    // With /*verilator unroll_full*/ still have a limit to avoid infinite loops
    if (full.isSetTrue()) count = std::max(count, count * 1024);
    if (generate) count = std::max(count, count * 16);
    if (simulate) count = std::max(count, count * 16);
    return count;
}

//######################################################################
// V3 Options utilities

string V3Options::argString(int argc, char** argv) {
    // Return list of arguments as simple string
    string opts;
    for (int i = 0; i < argc; ++i) {
        if (i != 0) opts += " ";
        opts += string{argv[i]};
    }
    return opts;
}

//######################################################################
// V3 Options Parsing

void V3Options::parseOpts(FileLine* fl, int argc, char** argv) VL_MT_DISABLED {
    // Save command line options
    for (int i = 0; i < argc; ++i) addLineArg(argv[i]);

    // Parse all options
    // Initial entry point from Verilator.cpp
    parseOptsList(fl, ".", argc, argv);

    // Default certain options and error check
    // Detailed error, since this is what we often get when run with minimal arguments
    if (vFiles().empty()) {
        v3fatal("verilator: No Input Verilog file specified on command line, "
                "see verilator --help for more information\n");
    }

    // Default prefix to the filename
    if (prefix() == "" && topModule() != "") m_prefix = "V"s + AstNode::encodeName(topModule());
    if (prefix() == "" && vFiles().size() >= 1)
        m_prefix
            = "V"s + AstNode::encodeName(V3Os::filenameNonDirExt(vFiles().begin()->filename()));
    if (modPrefix() == "") m_modPrefix = prefix();

    // Find files in makedir
    addIncDirFallback(makeDir());
}

//======================================================================

bool V3Options::suffixed(const string& sw, const char* arg) {
    if (std::strlen(arg) > sw.length()) return false;
    return (0 == std::strcmp(sw.c_str() + sw.length() - std::strlen(arg), arg));
}

void V3Options::parseOptsList(FileLine* fl, const string& optdir, int argc,
                              char** argv) VL_MT_DISABLED {
    // Parse parameters
    // Note argc and argv DO NOT INCLUDE the filename in [0]!!!
    // May be called recursively when there are -f files.
    for (int i = 0; i < argc; ++i) {
        addArg(argv[i]);  // -f's really should be inserted in the middle, but this is for debug
    }

    V3OptionParser parser;
    const V3OptionParser::AppendHelper DECL_OPTION{parser};
    V3OPTION_PARSER_DECL_TAGS;

    const auto callStrSetter = [this](void (V3Options::*cbStr)(const string&)) {
        return [this, cbStr](const string& v) { (this->*cbStr)(v); };
    };
    // Usage
    // DECL_OPTION("-option", action, pointer_or_lambda);
    // action: one of Set, OnOff, CbCall, CbOnOff, CbVal, CbPartialMatch, and CbPartialMatchVal
    //   Set              : Set value to a variable, pointer_or_lambda must be a pointer to the
    //                      variable.
    //                      true is set to bool-ish variable when '-opt' is passed to verilator.
    //                      val is set to int and string variable when '-opt val' is passed.
    //   OnOff            : Set value to a bool-ish variable, pointer_or_lambda must be a pointer
    //                      to bool or VOptionBool.
    //                      true is set if "-opt" is passed to verilator while false is set if
    //                      "-no-opt" is given.
    //   CbCall           : Call lambda or function that does not take argument.
    //   CbOnOff          : Call lambda or function that takes bool argument.
    //                      Supports "-opt" and "-no-opt" style options.
    //   CbVal            : Call lambda or function that takes int or const char*.
    //                      "-opt val" is passed to verilator, val is passed to the lambda.
    //                      If a function to be called is a member of V3Options that only takes
    //                      const string&, callStrSetter(&V3Options::memberFunc) can be passed
    //                      instead of lambda as a syntax sugar.
    //   CbPartialMatch   : Call lambda or function that takes remaining string.
    //                      e.g. DECL_OPTION("-opt-", CbPartialMatch, [](const char*optp) { cout <<
    //                      optp << endl; }); and "-opt-ABC" is passed, "ABC" will be emit to
    //                      stdout.
    //   CbPartialMatchVal: Call lambda or function that takes remaining string and value.
    //                      e.g. DECL_OPTION("-opt-", CbPartialMatchVal, [](const char*optp, const
    //                      char*valp) {
    //                               cout << optp << ":" << valp << endl; });
    //                      and "-opt-ABC VAL" is passed, "ABC:VAL" will be emit to stdout.
    //
    // DECL_OPTION is not C-macro to get correct line coverage even when lambda is passed.
    // (If DECL_OPTION is a macro, then lambda would be collapsed into a single line).

    // Plus options
    DECL_OPTION("+define+", CbPartialMatch,
                [this](const char* optp) VL_MT_DISABLED { addDefine(optp, true); });
    DECL_OPTION("+incdir+", CbPartialMatch, [this, &optdir](const char* optp) {
        string dirs = optp;
        string::size_type pos;
        while ((pos = dirs.find('+')) != string::npos) {
            addIncDirUser(parseFileArg(optdir, dirs.substr(0, pos)));
            dirs = dirs.substr(pos + 1);
        }
        addIncDirUser(parseFileArg(optdir, dirs));
    });
    DECL_OPTION("+libext+", CbPartialMatch, [this](const char* optp) {
        string exts = optp;
        string::size_type pos;
        while ((pos = exts.find('+')) != string::npos) {
            addLibExtV(exts.substr(0, pos));
            exts = exts.substr(pos + 1);
        }
        addLibExtV(exts);
    });
    DECL_OPTION("+librescan", CbCall, []() {});  // NOP
    DECL_OPTION("+notimingchecks", CbCall, []() {});  // NOP
    DECL_OPTION("+systemverilogext+", CbPartialMatch,
                [this](const char* optp) { addLangExt(optp, V3LangCode::L1800_2017); });
    DECL_OPTION("+verilog1995ext+", CbPartialMatch,
                [this](const char* optp) { addLangExt(optp, V3LangCode::L1364_1995); });
    DECL_OPTION("+verilog2001ext+", CbPartialMatch,
                [this](const char* optp) { addLangExt(optp, V3LangCode::L1364_2001); });
    DECL_OPTION("+1364-1995ext+", CbPartialMatch,
                [this](const char* optp) { addLangExt(optp, V3LangCode::L1364_1995); });
    DECL_OPTION("+1364-2001ext+", CbPartialMatch,
                [this](const char* optp) { addLangExt(optp, V3LangCode::L1364_2001); });
    DECL_OPTION("+1364-2005ext+", CbPartialMatch,
                [this](const char* optp) { addLangExt(optp, V3LangCode::L1364_2005); });
    DECL_OPTION("+1800-2005ext+", CbPartialMatch,
                [this](const char* optp) { addLangExt(optp, V3LangCode::L1800_2005); });
    DECL_OPTION("+1800-2009ext+", CbPartialMatch,
                [this](const char* optp) { addLangExt(optp, V3LangCode::L1800_2009); });
    DECL_OPTION("+1800-2012ext+", CbPartialMatch,
                [this](const char* optp) { addLangExt(optp, V3LangCode::L1800_2012); });
    DECL_OPTION("+1800-2017ext+", CbPartialMatch,
                [this](const char* optp) { addLangExt(optp, V3LangCode::L1800_2017); });
    DECL_OPTION("+1800-2023ext+", CbPartialMatch,
                [this](const char* optp) { addLangExt(optp, V3LangCode::L1800_2023); });

    // Minus options
    DECL_OPTION("-assert", CbOnOff, [this](bool flag) {
        m_assert = flag;
        m_assertCase = flag;
    });
    DECL_OPTION("-assert-case", OnOff, &m_assertCase);
    DECL_OPTION("-autoflush", OnOff, &m_autoflush);

    DECL_OPTION("-bbox-sys", OnOff, &m_bboxSys);
    DECL_OPTION("-bbox-unsup", CbOnOff, [this](bool flag) {
        m_bboxUnsup = flag;
        FileLine::globalWarnOff(V3ErrorCode::E_UNSUPPORTED, true);
    });
    DECL_OPTION("-binary", CbCall, [this]() {
        m_binary = true;
        m_build = true;
        m_exe = true;
        m_main = true;
        if (m_timing.isDefault()) m_timing = VOptionBool::OPT_TRUE;
    });
    DECL_OPTION("-build", Set, &m_build);
    DECL_OPTION("-build-dep-bin", Set, &m_buildDepBin);
    DECL_OPTION("-build-jobs", CbVal, [this, fl](const char* valp) {
        int val = std::atoi(valp);
        if (val < 0) {
            fl->v3error("--build-jobs requires a non-negative integer, but '" << valp
                                                                              << "' was passed");
            val = 1;
        } else if (val == 0) {
            val = std::thread::hardware_concurrency();
        }
        m_buildJobs = val;
    });

    DECL_OPTION("-CFLAGS", CbVal, callStrSetter(&V3Options::addCFlags));
    DECL_OPTION("-cc", CbCall, [this]() { ccSet(); });
    DECL_OPTION("-clk", CbVal, callStrSetter(&V3Options::addClocker));
    DECL_OPTION("-no-clk", CbVal, callStrSetter(&V3Options::addNoClocker));
    DECL_OPTION("-comp-limit-blocks", Set, &m_compLimitBlocks).undocumented();
    DECL_OPTION("-comp-limit-members", Set,
                &m_compLimitMembers)
        .undocumented();  // Ideally power-of-two so structs stay aligned
    DECL_OPTION("-comp-limit-parens", Set, &m_compLimitParens).undocumented();
    DECL_OPTION("-comp-limit-syms", CbVal, [](int val) { VName::maxLength(val); }).undocumented();
    DECL_OPTION("-compiler", CbVal, [this, fl](const char* valp) {
        if (!std::strcmp(valp, "clang")) {
            m_compLimitBlocks = 80;  // limit unknown
            m_compLimitMembers = 64;  // soft limit, has slowdown bug as of clang++ 3.8
            m_compLimitParens = 240;  // controlled by -fbracket-depth, which defaults to 256
        } else if (!std::strcmp(valp, "gcc")) {
            m_compLimitBlocks = 0;  // Bug free
            m_compLimitMembers = 64;  // soft limit, has slowdown bug as of g++ 7.1
            m_compLimitParens = 240;  // Unlimited, but generate same code as for clang
        } else if (!std::strcmp(valp, "msvc")) {
            m_compLimitBlocks = 80;  // 128, but allow some room
            m_compLimitMembers = 0;  // probably ok, and AFAIK doesn't support anon structs
            m_compLimitParens = 80;  // 128, but allow some room
        } else {
            fl->v3error("Unknown setting for --compiler: '"
                        << valp << "'\n"
                        << fl->warnMore() << "... Suggest 'clang', 'gcc', or 'msvc'");
        }
    });
    DECL_OPTION("-compiler-include", CbVal, callStrSetter(&V3Options::addCompilerIncludes));
    DECL_OPTION("-converge-limit", Set, &m_convergeLimit);
    DECL_OPTION("-coverage", CbOnOff, [this](bool flag) { coverage(flag); });
    DECL_OPTION("-coverage-expr", OnOff, &m_coverageExpr);
    DECL_OPTION("-coverage-expr-max", Set, &m_coverageExprMax);
    DECL_OPTION("-coverage-line", OnOff, &m_coverageLine);
    DECL_OPTION("-coverage-max-width", Set, &m_coverageMaxWidth);
    DECL_OPTION("-coverage-toggle", OnOff, &m_coverageToggle);
    DECL_OPTION("-coverage-underscore", OnOff, &m_coverageUnderscore);
    DECL_OPTION("-coverage-user", OnOff, &m_coverageUser);

    DECL_OPTION("-D", CbPartialMatch,
                [this](const char* valp) VL_MT_DISABLED { addDefine(valp, false); });
    DECL_OPTION("-debug", CbCall, [this]() { setDebugMode(3); });
    DECL_OPTION("-debugi", CbVal, [this](int v) { setDebugMode(v); });
    DECL_OPTION("-debugi-", CbPartialMatchVal, [this](const char* optp, const char* valp) {
        m_debugLevel[optp] = std::atoi(valp);
    });
    DECL_OPTION("-debug-abort", CbCall,
                V3Error::vlAbort)
        .undocumented();  // See also --debug-sigsegv
    DECL_OPTION("-debug-check", OnOff, &m_debugCheck);
    DECL_OPTION("-debug-collision", OnOff, &m_debugCollision).undocumented();
    DECL_OPTION("-debug-emitv", OnOff, &m_debugEmitV).undocumented();
    DECL_OPTION("-debug-exit-parse", OnOff, &m_debugExitParse).undocumented();
    DECL_OPTION("-debug-exit-uvm", OnOff, &m_debugExitUvm).undocumented();
    DECL_OPTION("-debug-exit-uvm23", OnOff, &m_debugExitUvm23).undocumented();
    DECL_OPTION("-debug-fatalsrc", CbCall, []() {
        v3fatalSrc("--debug-fatal-src");
    }).undocumented();  // See also --debug-abort
    DECL_OPTION("-debug-leak", OnOff, &m_debugLeak);
    DECL_OPTION("-debug-nondeterminism", OnOff, &m_debugNondeterminism);
    DECL_OPTION("-debug-partition", OnOff, &m_debugPartition).undocumented();
    DECL_OPTION("-debug-protect", OnOff, &m_debugProtect).undocumented();
    DECL_OPTION("-debug-self-test", OnOff, &m_debugSelfTest).undocumented();
    DECL_OPTION("-debug-sigsegv", CbCall, throwSigsegv).undocumented();  // See also --debug-abort
    DECL_OPTION("-debug-stack-check", OnOff, &m_debugStackCheck).undocumented();
    DECL_OPTION("-debug-width", OnOff, &m_debugWidth).undocumented();
    DECL_OPTION("-decoration", CbCall, [this, fl]() { decorations(fl, "medium"); });
    DECL_OPTION("-decorations", CbVal, [this, fl](const char* optp) { decorations(fl, optp); });
    DECL_OPTION("-no-decoration", CbCall, [this, fl]() { decorations(fl, "none"); });
    DECL_OPTION("-diagnostics-sarif", OnOff, &m_diagnosticsSarif);
    DECL_OPTION("-diagnostics-sarif-output", CbVal, [this](const char* optp) {
        m_diagnosticsSarifOutput = optp;
        m_diagnosticsSarif = true;
    });
    DECL_OPTION("-dpi-hdr-only", OnOff, &m_dpiHdrOnly);
    DECL_OPTION("-dump-", CbPartialMatch, [this](const char* optp) { m_dumpLevel[optp] = 3; });
    DECL_OPTION("-no-dump-", CbPartialMatch, [this](const char* optp) { m_dumpLevel[optp] = 0; });
    DECL_OPTION("-dumpi-", CbPartialMatchVal, [this](const char* optp, const char* valp) {
        m_dumpLevel[optp] = std::atoi(valp);
    });

    DECL_OPTION("-E", CbOnOff, [this](bool flag) {
        if (flag) {
            m_stdPackage = false;
            m_stdWaiver = false;
        }
        m_preprocOnly = flag;
    });
    DECL_OPTION("-emit-accessors", OnOff, &m_emitAccessors);
    DECL_OPTION("-error-limit", CbVal, static_cast<void (*)(int)>(&V3Error::errorLimit));
    DECL_OPTION("-exe", OnOff, &m_exe);
    DECL_OPTION("-expand-limit", CbVal,
                [this](const char* valp) { m_expandLimit = std::atoi(valp); });

    DECL_OPTION("-F", CbVal, [this, fl, &optdir](const char* valp) VL_MT_DISABLED {
        parseOptsFile(fl, parseFileArg(optdir, valp), true);
    });
    DECL_OPTION("-FI", CbVal,
                [this, &optdir](const char* valp) { addForceInc(parseFileArg(optdir, valp)); });
    DECL_OPTION("-f", CbVal, [this, fl, &optdir](const char* valp) VL_MT_DISABLED {
        parseOptsFile(fl, parseFileArg(optdir, valp), false);
    });
    DECL_OPTION("-flatten", OnOff, &m_flatten);
    DECL_OPTION("-future0", CbVal, [this](const char* valp) { addFuture0(valp); });
    DECL_OPTION("-future1", CbVal, [this](const char* valp) { addFuture1(valp); });

    DECL_OPTION("-facyc-simp", FOnOff, &m_fAcycSimp);
    DECL_OPTION("-fassemble", FOnOff, &m_fAssemble);
    DECL_OPTION("-fcase", FOnOff, &m_fCase);
    DECL_OPTION("-fcombine", FOnOff, &m_fCombine);
    DECL_OPTION("-fconst", FOnOff, &m_fConst);
    DECL_OPTION("-fconst-before-dfg", FOnOff, &m_fConstBeforeDfg);
    DECL_OPTION("-fconst-bit-op-tree", FOnOff, &m_fConstBitOpTree);
    DECL_OPTION("-fconst-eager", FOnOff, &m_fConstEager);
    DECL_OPTION("-fdead-assigns", FOnOff, &m_fDeadAssigns);
    DECL_OPTION("-fdead-cells", FOnOff, &m_fDeadCells);
    DECL_OPTION("-fdedup", FOnOff, &m_fDedupe);
    DECL_OPTION("-fdfg", CbFOnOff, [this](bool flag) {
        m_fDfgPreInline = flag;
        m_fDfgPostInline = flag;
        m_fDfgScoped = flag;
    });
    DECL_OPTION("-fdfg-break-cycles", FOnOff, &m_fDfgBreakCycles);
    DECL_OPTION("-fdfg-peephole", FOnOff, &m_fDfgPeephole);
    DECL_OPTION("-fdfg-peephole-", CbPartialMatch, [this](const char* optp) {  //
        m_fDfgPeepholeDisabled.erase(optp);
    });
    DECL_OPTION("-fno-dfg-peephole-", CbPartialMatch, [this](const char* optp) {  //
        m_fDfgPeepholeDisabled.emplace(optp);
    });
    DECL_OPTION("-fdfg-pre-inline", FOnOff, &m_fDfgPreInline);
    DECL_OPTION("-fdfg-post-inline", FOnOff, &m_fDfgPostInline);
    DECL_OPTION("-fdfg-scoped", FOnOff, &m_fDfgScoped);
    DECL_OPTION("-fexpand", FOnOff, &m_fExpand);
    DECL_OPTION("-ffunc-opt", CbFOnOff, [this](bool flag) {  //
        m_fFuncSplitCat = flag;
        m_fFuncBalanceCat = flag;
    });
    DECL_OPTION("-ffunc-opt-balance-cat", FOnOff, &m_fFuncBalanceCat);
    DECL_OPTION("-ffunc-opt-split-cat", FOnOff, &m_fFuncSplitCat);
    DECL_OPTION("-fgate", FOnOff, &m_fGate);
    DECL_OPTION("-finline", FOnOff, &m_fInline);
    DECL_OPTION("-finline-funcs", FOnOff, &m_fInlineFuncs);
    DECL_OPTION("-flife", FOnOff, &m_fLife);
    DECL_OPTION("-flife-post", FOnOff, &m_fLifePost);
    DECL_OPTION("-flocalize", FOnOff, &m_fLocalize);
    DECL_OPTION("-fmerge-cond", FOnOff, &m_fMergeCond);
    DECL_OPTION("-fmerge-cond-motion", FOnOff, &m_fMergeCondMotion);
    DECL_OPTION("-fmerge-const-pool", FOnOff, &m_fMergeConstPool);
    DECL_OPTION("-freloop", FOnOff, &m_fReloop);
    DECL_OPTION("-freorder", FOnOff, &m_fReorder);
    DECL_OPTION("-fslice", FOnOff, &m_fSlice);
    DECL_OPTION("-fsplit", FOnOff, &m_fSplit);
    DECL_OPTION("-fsubst", FOnOff, &m_fSubst);
    DECL_OPTION("-fsubst-const", FOnOff, &m_fSubstConst);
    DECL_OPTION("-ftable", FOnOff, &m_fTable);
    DECL_OPTION("-ftaskify-all-forked", FOnOff, &m_fTaskifyAll).undocumented();  // Debug
    DECL_OPTION("-fvar-split", FOnOff, &m_fVarSplit);

    DECL_OPTION("-G", CbPartialMatch, [this](const char* optp) { addParameter(optp, false); });
    DECL_OPTION("-gate-stmts", Set, &m_gateStmts);
    DECL_OPTION("-gdb", CbCall, []() {});  // Processed only in bin/verilator shell
    DECL_OPTION("-gdbbt", CbCall, []() {});  // Processed only in bin/verilator shell
    DECL_OPTION("-generate-key", CbCall, [this]() {
        cout << protectKeyDefaulted() << endl;
        std::exit(0);
    });
    DECL_OPTION("-getenv", CbVal, [](const char* valp) {
        cout << V3Options::getenvBuiltins(valp) << endl;
        std::exit(0);
    });
    DECL_OPTION("-get-supported", CbVal, [](const char* valp) {
        cout << V3Options::getSupported(valp) << endl;
        std::exit(0);
    });

    DECL_OPTION("-hierarchical", OnOff, &m_hierarchical);
    DECL_OPTION("-hierarchical-block", CbVal, [this](const char* valp) {
        const V3HierarchicalBlockOption opt{valp};
        m_hierBlocks.emplace(opt.mangledName(), opt);
    });
    DECL_OPTION("-hierarchical-child", Set, &m_hierChild);
    DECL_OPTION("-hierarchical-params-file", CbVal, [this](const char* optp) {
        m_hierParamsFile.push_back({optp, work()});
    });

    DECL_OPTION("-I", CbPartialMatch,
                [this, &optdir](const char* optp) { addIncDirUser(parseFileArg(optdir, optp)); });
    DECL_OPTION("-if-depth", Set, &m_ifDepth);
    DECL_OPTION("-ignc", OnOff, &m_ignc);
    DECL_OPTION("-inline-mult", Set, &m_inlineMult);
    DECL_OPTION("-instr-count-dpi", CbVal, [this, fl](int val) {
        m_instrCountDpi = val;
        if (m_instrCountDpi < 0) fl->v3fatal("--instr-count-dpi must be non-negative: " << val);
    });

    DECL_OPTION("-json-edit-nums", OnOff, &m_jsonEditNums);
    DECL_OPTION("-json-ids", OnOff, &m_jsonIds);
    DECL_OPTION("-json-only", OnOff, &m_jsonOnly);
    DECL_OPTION("-json-only-meta-output", CbVal, [this](const char* valp) {
        m_jsonOnlyMetaOutput = valp;
        m_jsonOnly = true;
    });
    DECL_OPTION("-json-only-output", CbVal, [this](const char* valp) {
        m_jsonOnlyOutput = valp;
        m_jsonOnly = true;
    });

    DECL_OPTION("-LDFLAGS", CbVal, callStrSetter(&V3Options::addLdLibs));
    DECL_OPTION("-l2-name", Set, &m_l2Name);
    DECL_OPTION("-no-l2name", CbCall, [this]() { m_l2Name = ""; }).undocumented();  // Historical
    DECL_OPTION("-l2name", CbCall, [this]() { m_l2Name = "v"; }).undocumented();  // Historical
    const auto setLang = [this, fl](const char* valp) {
        const V3LangCode optval{valp};
        if (optval.legal()) {
            m_defaultLanguage = optval;
        } else {
            VSpellCheck spell;
            for (int i = V3LangCode::L_ERROR + 1; i < V3LangCode::_ENUM_END; ++i) {
                spell.pushCandidate(V3LangCode{i}.ascii());
            }
            fl->v3error("Unknown language specified: " << valp << spell.bestCandidateMsg(valp));
        }
    };
    DECL_OPTION("-default-language", CbVal, setLang);
    DECL_OPTION("-language", CbVal, setLang);
    DECL_OPTION("-lib-create", CbVal, [this, fl](const char* valp) {
        validateIdentifier(fl, valp, "--lib-create");
        m_libCreate = valp;
    });
    DECL_OPTION("-lint-only", OnOff, &m_lintOnly);
    DECL_OPTION("-localize-max-size", Set, &m_localizeMaxSize);

    DECL_OPTION("-MAKEFLAGS", CbVal, callStrSetter(&V3Options::addMakeFlags));
    DECL_OPTION("-MMD", OnOff, &m_makeDepend);
    DECL_OPTION("-MP", OnOff, &m_makePhony);
    DECL_OPTION("-Mdir", CbVal, [this](const char* valp) {
        m_makeDir = valp;
        addIncDirFallback(m_makeDir);  // Need to find generated files there too
    });
    DECL_OPTION("-main", OnOff, &m_main);
    DECL_OPTION("-main-top-name", Set, &m_mainTopName);
    DECL_OPTION("-make", CbVal, [this, fl](const char* valp) {
        if (!std::strcmp(valp, "cmake")) {
            m_cmake = true;
        } else if (!std::strcmp(valp, "gmake")) {
            m_gmake = true;
        } else if (!std::strcmp(valp, "json")) {
            m_makeJson = true;
        } else {
            fl->v3error("Unknown --make system specified: '" << valp << "'");
        }
    });
    DECL_OPTION("-max-num-width", Set, &m_maxNumWidth);
    DECL_OPTION("-mod-prefix", CbVal, [this, fl](const char* valp) {
        validateIdentifier(fl, valp, "--mod-prefix");
        m_modPrefix = valp;
    });

    DECL_OPTION("-O0", CbCall, [this]() { optimize(0); });
    DECL_OPTION("-O1", CbCall, [this]() { optimize(1); });
    DECL_OPTION("-O2", CbCall, [this]() { optimize(2); });
    DECL_OPTION("-O3", CbCall, [this]() { optimize(3); });

    DECL_OPTION("-o", Set, &m_exeName);
    DECL_OPTION("-order-clock-delay", CbOnOff, [fl](bool /*flag*/) {
        fl->v3warn(DEPRECATED, "Option order-clock-delay is deprecated and has no effect.");
    });
    DECL_OPTION("-output-groups", CbVal, [this, fl](const char* valp) {
        m_outputGroups = std::atoi(valp);
        if (m_outputGroups < -1) fl->v3error("--output-groups must be >= -1: " << valp);
    });
    DECL_OPTION("-output-split", Set, &m_outputSplit);
    DECL_OPTION("-output-split-cfuncs", CbVal, [this, fl](const char* valp) {
        m_outputSplitCFuncs = std::atoi(valp);
        if (m_outputSplitCFuncs < 0) {
            fl->v3error("--output-split-cfuncs must be >= 0: " << valp);
        }
    });
    DECL_OPTION("-output-split-ctrace", CbVal, [this, fl](const char* valp) {
        m_outputSplitCTrace = std::atoi(valp);
        if (m_outputSplitCTrace < 0) {
            fl->v3error("--output-split-ctrace must be >= 0: " << valp);
        }
    });

    DECL_OPTION("-P", Set, &m_preprocNoLine);
    DECL_OPTION("-pins64", CbCall, [this]() { m_pinsBv = 65; });
    DECL_OPTION("-no-pins64", CbCall, [this]() { m_pinsBv = 33; });
    DECL_OPTION("-pins-bv", CbVal, [this, fl](const char* valp) {
        m_pinsBv = std::atoi(valp);
        if (m_pinsBv > 65) fl->v3error("--pins-bv maximum is 65: " << valp);
    });
    DECL_OPTION("-pins-inout-enables", OnOff, &m_pinsInoutEnables);
    DECL_OPTION("-pins-sc-uint", CbOnOff, [this](bool flag) {
        m_pinsScUint = flag;
        if (!m_pinsScBigUint) m_pinsBv = 65;
    });
    DECL_OPTION("-pins-sc-uint-bool", CbOnOff, [this](bool flag) { m_pinsScUintBool = flag; });
    DECL_OPTION("-pins-sc-biguint", CbOnOff, [this](bool flag) {
        m_pinsScBigUint = flag;
        m_pinsBv = 513;
    });
    DECL_OPTION("-pins-uint8", OnOff, &m_pinsUint8);
    DECL_OPTION("-pipe-filter", Set, &m_pipeFilter);
    DECL_OPTION("-pp-comments", OnOff, &m_ppComments);
    DECL_OPTION("-prefix", CbVal, [this, fl](const char* valp) {
        validateIdentifier(fl, valp, "--prefix");
        m_prefix = valp;
    });
    DECL_OPTION("-preproc-resolve", OnOff, &m_preprocResolve);
    DECL_OPTION("-preproc-token-limit", CbVal, [this, fl](const char* valp) {
        m_preprocTokenLimit = std::atoi(valp);
        if (m_preprocTokenLimit <= 0) fl->v3error("--preproc-token-limit must be > 0: " << valp);
    });
    DECL_OPTION("-private", CbCall, [this]() { m_public = false; });
    DECL_OPTION("-prof-c", OnOff, &m_profC);
    DECL_OPTION("-prof-cfuncs", CbCall, [this]() { m_profC = m_profCFuncs = true; });
    DECL_OPTION("-prof-exec", OnOff, &m_profExec);
    DECL_OPTION("-prof-pgo", OnOff, &m_profPgo);
    DECL_OPTION("-profile-cfuncs", CbCall,
                [this]() { m_profC = m_profCFuncs = true; });  // Renamed
    DECL_OPTION("-protect-ids", OnOff, &m_protectIds);
    DECL_OPTION("-protect-key", Set, &m_protectKey);
    DECL_OPTION("-protect-lib", CbVal, [this, fl](const char* valp) {
        validateIdentifier(fl, valp, "--protect-lib");
        m_libCreate = valp;
        m_protectIds = true;
    });
    DECL_OPTION("-public", OnOff, &m_public);
    DECL_OPTION("-public-depth", Set, &m_publicDepth);
    DECL_OPTION("-public-flat-rw", CbOnOff, [this](bool flag) {
        m_publicFlatRW = flag;
        v3Global.dpi(true);
    });
    DECL_OPTION("-public-ignore", CbOnOff, [this](bool flag) { m_publicIgnore = flag; });
    DECL_OPTION("-public-params", CbOnOff, [this](bool flag) {
        m_publicParams = flag;
        v3Global.dpi(true);
    });
    DECL_OPTION("-pvalue+", CbPartialMatch,
                [this](const char* varp) { addParameter(varp, false); });

    DECL_OPTION("-quiet", CbOnOff, [this](bool flag) {
        m_quietExit = flag;
        m_quietStats = flag;
    });
    DECL_OPTION("-quiet-exit", OnOff, &m_quietExit);
    DECL_OPTION("-quiet-stats", OnOff, &m_quietStats);

    DECL_OPTION("-relative-includes", OnOff, &m_relativeIncludes);
    DECL_OPTION("-reloop-limit", CbVal, [this, fl](const char* valp) {
        m_reloopLimit = std::atoi(valp);
        if (m_reloopLimit < 2) fl->v3error("--reloop-limit must be >= 2: " << valp);
    });
    DECL_OPTION("-report-unoptflat", OnOff, &m_reportUnoptflat);
    DECL_OPTION("-rr", CbCall, []() {});  // Processed only in bin/verilator shell
    DECL_OPTION("-runtime-debug", CbCall, [this, fl]() {
        decorations(fl, "node");
        addCFlags("-ggdb");
        addLdLibs("-ggdb");
        addCFlags("-fsanitize=address,undefined");
        addLdLibs("-fsanitize=address,undefined");
        addCFlags("-D_GLIBCXX_DEBUG");
        addCFlags("-DVL_DEBUG=1");
    });

    DECL_OPTION("-savable", OnOff, &m_savable);
    DECL_OPTION("-sc", CbCall, [this]() {
        m_outFormatOk = true;
        m_systemC = true;
    });
    DECL_OPTION("-skip-identical", OnOff, &m_skipIdentical);
    DECL_OPTION("-stats", OnOff, &m_stats);
    DECL_OPTION("-stats-vars", CbOnOff, [this](bool flag) {
        m_statsVars = flag;
        m_stats |= flag;
    });
    DECL_OPTION("-std", CbOnOff, [this](bool flag) {
        m_stdPackage = flag;
        m_stdWaiver = flag;
    });
    DECL_OPTION("-std-package", OnOff, &m_stdPackage);
    DECL_OPTION("-std-waiver", OnOff, &m_stdWaiver);
    DECL_OPTION("-stop-fail", OnOff, &m_stopFail);
    DECL_OPTION("-structs-packed", OnOff, &m_structsPacked);
    DECL_OPTION("-sv", CbCall, [this]() { m_defaultLanguage = V3LangCode::L1800_2023; });

    DECL_OPTION("-no-threads", CbCall, [this, fl]() {
        fl->v3warn(DEPRECATED, "Option --no-threads is deprecated, use '--threads 1' instead");
        m_threads = 1;
    });
    DECL_OPTION("-threads", CbVal, [this, fl](const char* valp) {
        m_threads = std::atoi(valp);
        if (m_threads < 0) fl->v3fatal("--threads must be >= 0: " << valp);
        if (m_threads == 0) {
            fl->v3warn(DEPRECATED, "Option --threads 0 is deprecated, use '--threads 1' instead");
            m_threads = 1;
        }
    });
    DECL_OPTION("-hierarchical-threads", CbVal, [this, fl](const char* valp) {
        m_hierThreads = std::atoi(valp);
        if (m_hierThreads < 0) fl->v3fatal("--hierarchical-threads must be >= 0: " << valp);
    });
    DECL_OPTION("-threads-coarsen", OnOff, &m_threadsCoarsen).undocumented();  // Debug
    DECL_OPTION("-threads-dpi", CbVal, [this, fl](const char* valp) {
        if (!std::strcmp(valp, "all")) {
            m_threadsDpiPure = true;
            m_threadsDpiUnpure = true;
        } else if (!std::strcmp(valp, "none")) {
            m_threadsDpiPure = false;
            m_threadsDpiUnpure = false;
        } else if (!std::strcmp(valp, "pure")) {
            m_threadsDpiPure = true;
            m_threadsDpiUnpure = false;
        } else {
            fl->v3error("Unknown setting for --threads-dpi: '"
                        << valp << "'\n"
                        << fl->warnMore() << "... Suggest 'all', 'none', or 'pure'");
        }
    });
    DECL_OPTION("-threads-max-mtasks", CbVal, [this, fl](const char* valp) {
        m_threadsMaxMTasks = std::atoi(valp);
        if (m_threadsMaxMTasks < 1) fl->v3fatal("--threads-max-mtasks must be >= 1: " << valp);
    });
    DECL_OPTION("-timescale", CbVal, [this, fl](const char* valp) {
        VTimescale unit;
        VTimescale prec;
        VTimescale::parseSlashed(fl, valp, unit /*ref*/, prec /*ref*/);
        if (!unit.isNone() && timeOverrideUnit().isNone()) m_timeDefaultUnit = unit;
        if (!prec.isNone() && timeOverridePrec().isNone()) m_timeDefaultPrec = prec;
    });
    DECL_OPTION("-timescale-override", CbVal, [this, fl](const char* valp) {
        VTimescale unit;
        VTimescale prec;
        VTimescale::parseSlashed(fl, valp, unit /*ref*/, prec /*ref*/, true);
        if (!unit.isNone()) {
            m_timeDefaultUnit = unit;
            m_timeOverrideUnit = unit;
        }
        if (!prec.isNone()) {
            m_timeDefaultPrec = prec;
            m_timeOverridePrec = prec;
        }
    });
    DECL_OPTION("-timing", OnOff, &m_timing);
    DECL_OPTION("-top", Set, &m_topModule);
    DECL_OPTION("-top-module", Set, &m_topModule);
    DECL_OPTION("-trace", OnOff, &m_trace);
    DECL_OPTION("-trace-saif", CbCall, [this]() {
        m_trace = true;
        m_traceFormat = TraceFormat::SAIF;
    });
    DECL_OPTION("-trace-coverage", OnOff, &m_traceCoverage);
    DECL_OPTION("-trace-depth", Set, &m_traceDepth);
    DECL_OPTION("-trace-fst", CbCall, [this]() {
        m_trace = true;
        m_traceFormat = TraceFormat::FST;
        addLdLibs("-lz");
    });
    DECL_OPTION("-trace-fst-thread", CbCall, [this, fl]() {
        m_trace = true;
        m_traceFormat = TraceFormat::FST;
        addLdLibs("-lz");
        fl->v3warn(DEPRECATED, "Option --trace-fst-thread is deprecated. "
                               "Use --trace-fst with --trace-threads > 0.");
        if (m_traceThreads == 0) m_traceThreads = 1;
    });
    DECL_OPTION("-trace-max-array", Set, &m_traceMaxArray);
    DECL_OPTION("-trace-max-width", Set, &m_traceMaxWidth);
    DECL_OPTION("-trace-params", OnOff, &m_traceParams);
    DECL_OPTION("-trace-structs", OnOff, &m_traceStructs);
    DECL_OPTION("-trace-threads", CbVal, [this, fl](const char* valp) {
        m_trace = true;
        m_traceThreads = std::atoi(valp);
        if (m_traceThreads < 1) fl->v3fatal("--trace-threads must be >= 1: " << valp);
    });
    DECL_OPTION("-no-trace-top", Set, &m_noTraceTop);
    DECL_OPTION("-trace-underscore", OnOff, &m_traceUnderscore);
    DECL_OPTION("-trace-vcd", CbCall, [this]() {
        m_trace = true;
        m_traceFormat = TraceFormat::VCD;
    });

    DECL_OPTION("-U", CbPartialMatch, &V3PreShell::undef);
    DECL_OPTION("-underline-zero", OnOff, &m_underlineZero);  // Deprecated
    DECL_OPTION("-no-unlimited-stack", CbCall, []() {});  // Processed only in bin/verilator shell
    DECL_OPTION("-unroll-count", Set, &m_unrollCount).undocumented();  // Optimization tweak
    DECL_OPTION("-unroll-stmts", Set, &m_unrollStmts).undocumented();  // Optimization tweak
    DECL_OPTION("-unused-regexp", Set, &m_unusedRegexp);

    DECL_OPTION("-V", CbCall, [this]() {
        showVersion(true);
        std::exit(0);
    });
    DECL_OPTION("-v", CbVal, [this, &optdir](const char* valp) {
        V3Options::addLibraryFile(parseFileArg(optdir, valp), work());
    });
    DECL_OPTION("-valgrind", CbCall, []() {});  // Processed only in bin/verilator shell
    DECL_OPTION("-verilate", OnOff, &m_verilate);
    DECL_OPTION("-verilate-jobs", CbVal, [this, fl](const char* valp) {
        int val = std::atoi(valp);
        if (val < 0) {
            fl->v3error("--verilate-jobs requires a non-negative integer, but '"
                        << valp << "' was passed");
            val = 1;
        } else if (val == 0) {
            val = std::thread::hardware_concurrency();
        }
        m_verilateJobs = val;
    });
    DECL_OPTION("-version", CbCall, [this]() {
        showVersion(false);
        std::exit(0);
    });
    DECL_OPTION("-vpi", OnOff, &m_vpi);

    DECL_OPTION("-Wall", CbCall, []() {
        FileLine::globalWarnLintOff(false);
        FileLine::globalWarnStyleOff(false);
    });
    DECL_OPTION("-Werror-UNUSED", CbCall, []() {
        V3Error::pretendError(V3ErrorCode::UNUSEDGENVAR, true);
        V3Error::pretendError(V3ErrorCode::UNUSEDLOOP, true);
        V3Error::pretendError(V3ErrorCode::UNUSEDPARAM, true);
        V3Error::pretendError(V3ErrorCode::UNUSEDSIGNAL, true);
    });
    DECL_OPTION("-Werror-", CbPartialMatch, [this, fl](const char* optp) {
        const V3ErrorCode code{optp};
        if (code == V3ErrorCode::EC_ERROR) {
            if (!isFuture(optp)) fl->v3fatal("Unknown warning specified: -Werror-" << optp);
        } else {
            V3Error::pretendError(code, true);
        }
    });
    DECL_OPTION("-Wfuture-", CbPartialMatch, [this](const char* optp) {
        // Note it may not be a future option, but one that is currently implemented.
        addFuture(optp);
    });
    DECL_OPTION("-Wno-", CbPartialMatch, [fl, &parser](const char* optp) VL_MT_DISABLED {
        if (!FileLine::globalWarnOff(optp, true)) {
            const string fullopt = "-Wno-"s + optp;
            fl->v3fatal("Unknown warning specified: " << fullopt
                                                      << parser.getSuggestion(fullopt.c_str()));
        }
    });
    for (int i = V3ErrorCode::EC_FIRST_WARN; i < V3ErrorCode::_ENUM_MAX; ++i) {
        for (const string prefix : {"-Wno-", "-Wwarn-"})
            parser.addSuggestionCandidate(prefix + V3ErrorCode{i}.ascii());
    }
    DECL_OPTION("-Wno-context", CbCall, [this]() { m_context = false; });
    DECL_OPTION("-Wno-fatal", CbCall, []() { V3Error::warnFatal(false); });
    DECL_OPTION("-Wno-lint", CbCall, []() {
        FileLine::globalWarnLintOff(true);
        FileLine::globalWarnStyleOff(true);
    });
    DECL_OPTION("-Wno-style", CbCall, []() { FileLine::globalWarnStyleOff(true); });
    DECL_OPTION("-Wno-UNUSED", CbCall, []() { FileLine::globalWarnUnusedOff(true); });
    DECL_OPTION("-Wno-WIDTH", CbCall, []() { FileLine::globalWarnOff(V3ErrorCode::WIDTH, true); });
    DECL_OPTION("-work", Set, &m_work);
    DECL_OPTION("-Wpedantic", CbCall, [this]() {
        m_pedantic = true;
        V3Error::pretendError(V3ErrorCode::ASSIGNIN, false);
    });
    DECL_OPTION("-Wwarn-", CbPartialMatch, [this, fl, &parser](const char* optp) VL_MT_DISABLED {
        const V3ErrorCode code{optp};
        if (code == V3ErrorCode::EC_ERROR) {
            if (!isFuture(optp)) {
                const string fullopt = "-Wwarn-"s + optp;
                fl->v3fatal("Unknown warning specified: "
                            << fullopt << parser.getSuggestion(fullopt.c_str()));
            }
        } else {
            FileLine::globalWarnOff(code, false);
            V3Error::pretendError(code, false);
        }
    });
    DECL_OPTION("-Wwarn-lint", CbCall, []() { FileLine::globalWarnLintOff(false); });
    DECL_OPTION("-Wwarn-style", CbCall, []() { FileLine::globalWarnStyleOff(false); });
    DECL_OPTION("-Wwarn-UNUSED", CbCall, []() {
        FileLine::globalWarnUnusedOff(false);
        V3Error::pretendError(V3ErrorCode::UNUSEDGENVAR, false);
        V3Error::pretendError(V3ErrorCode::UNUSEDLOOP, false);
        V3Error::pretendError(V3ErrorCode::UNUSEDSIGNAL, false);
        V3Error::pretendError(V3ErrorCode::UNUSEDPARAM, false);
    });
    DECL_OPTION("-Wwarn-UNSUPPORTED", CbCall, []() {
        FileLine::globalWarnOff(V3ErrorCode::E_UNSUPPORTED, false);
        FileLine::globalWarnOff(V3ErrorCode::COVERIGN, false);
        FileLine::globalWarnOff(V3ErrorCode::SPECIFYIGN, false);
        V3Error::pretendError(V3ErrorCode::E_UNSUPPORTED, false);
        V3Error::pretendError(V3ErrorCode::COVERIGN, false);
        V3Error::pretendError(V3ErrorCode::SPECIFYIGN, false);
    });
    DECL_OPTION("-Wwarn-WIDTH", CbCall, []() {
        FileLine::globalWarnOff(V3ErrorCode::WIDTH, false);
        V3Error::pretendError(V3ErrorCode::WIDTH, false);
    });
    DECL_OPTION("-waiver-multiline", OnOff, &m_waiverMultiline);
    DECL_OPTION("-waiver-output", Set, &m_waiverOutput);

    DECL_OPTION("-x-assign", CbVal, [this, fl](const char* valp) {
        if (!std::strcmp(valp, "0")) {
            m_xAssign = "0";
        } else if (!std::strcmp(valp, "1")) {
            m_xAssign = "1";
        } else if (!std::strcmp(valp, "fast")) {
            m_xAssign = "fast";
        } else if (!std::strcmp(valp, "unique")) {
            m_xAssign = "unique";
        } else {
            fl->v3error("Unknown setting for --x-assign: '"
                        << valp << "'\n"
                        << fl->warnMore() << "... Suggest '0', '1', 'fast', or 'unique'");
        }
    });
    DECL_OPTION("-x-initial", CbVal, [this, fl](const char* valp) {
        if (!std::strcmp(valp, "0")) {
            m_xInitial = "0";
        } else if (!std::strcmp(valp, "fast")) {
            m_xInitial = "fast";
        } else if (!std::strcmp(valp, "unique")) {
            m_xInitial = "unique";
        } else {
            fl->v3error("Unknown setting for --x-initial: '"
                        << valp << "'\n"
                        << fl->warnMore() << "... Suggest '0', 'fast', or 'unique'");
        }
    });
    DECL_OPTION("-x-initial-edge", OnOff, &m_xInitialEdge);
    DECL_OPTION("-xml-only", CbOnOff, [this, fl](bool flag) {
        if (!m_xmlOnly && flag)
            fl->v3warn(DEPRECATED, "Option --xml-only is deprecated, move to --json-only");
        m_xmlOnly = flag;
    });
    DECL_OPTION("-xml-output", CbVal, [this, fl](const char* valp) {
        if (!m_xmlOnly)
            fl->v3warn(DEPRECATED, "Option --xml-only is deprecated, move to --json-only");
        m_xmlOutput = valp;
        m_xmlOnly = true;
    });

    DECL_OPTION("-y", CbVal, [this, &optdir](const char* valp) {
        addIncDirUser(parseFileArg(optdir, string{valp}));
    });

    parser.finalize();

    for (int i = 0; i < argc;) {
        UINFO(9, " Option: " << argv[i]);
        if (!std::strcmp(argv[i], "-j")
            || !std::strcmp(argv[i], "--j")) {  // Allow gnu -- switches
            ++i;
            int val = 0;
            if (i < argc && std::isdigit(argv[i][0])) {
                val = std::atoi(argv[i]);  // Can't be negative due to isdigit above
                if (val == 0) val = std::thread::hardware_concurrency();
                ++i;
            }
            if (m_buildJobs == -1) m_buildJobs = val;
            if (m_verilateJobs == -1) m_verilateJobs = val;
            if (m_outputGroups == -1) m_outputGroups = val;
        } else if (argv[i][0] == '-' || argv[i][0] == '+') {
            const char* argvNoDashp = (argv[i][1] == '-') ? (argv[i] + 2) : (argv[i] + 1);
            if (const int consumed = parser.parse(i, argc, argv)) {
                i += consumed;
            } else if (isFuture0(argvNoDashp)) {
                ++i;
            } else if (isFuture1(argvNoDashp)) {
                i += 2;
            } else {
                fl->v3fatal("Invalid option: " << argv[i] << parser.getSuggestion(argv[i]));
                ++i;  // LCOV_EXCL_LINE
            }
        } else {
            // Filename
            const string filename = parseFileArg(optdir, argv[i]);
            if (suffixed(filename, ".cpp")  //
                || suffixed(filename, ".cxx")  //
                || suffixed(filename, ".cc")  //
                || suffixed(filename, ".c")  //
                || suffixed(filename, ".sp")) {
                V3Options::addCppFile(filename);
            } else if (suffixed(filename, ".a")  //
                       || suffixed(filename, ".o")  //
                       || suffixed(filename, ".so")) {
                V3Options::addLdLibs(filename);
            } else if (suffixed(filename, ".vlt")) {
                V3Options::addVltFile(filename, work());
            } else {
                V3Options::addVFile(filename, work());
            }
            ++i;
        }
    }
}

//======================================================================

void V3Options::parseOptsFile(FileLine* fl, const string& filename, bool rel) VL_MT_DISABLED {
    // Read the specified -f filename and process as arguments
    UINFO(1, "Reading Options File " << filename);

    const std::unique_ptr<std::ifstream> ifp{V3File::new_ifstream(filename)};
    if (ifp->fail()) {
        fl->v3error("Cannot open -f command file: " + filename);
        return;
    }

    string whole_file;
    bool inCmt = false;
    while (!ifp->eof()) {
        const string line = V3Os::getline(*ifp);
        // Strip simple comments
        string oline;
        // cppcheck-suppress StlMissingComparison
        char lastch = ' ';
        bool space_begin = true;  // At beginning or leading spaces only
        for (string::const_iterator pos = line.begin(); pos != line.end(); lastch = *pos++) {
            if (inCmt) {
                if (*pos == '*' && *(pos + 1) == '/') {
                    inCmt = false;
                    ++pos;
                }
            } else if (*pos == '/' && *(pos + 1) == '/'
                       && (pos == line.begin()
                           || std::isspace(lastch))) {  // But allow /file//path
                break;  // Ignore to EOL
            } else if (*pos == '#' && space_begin) {  // Only # at [spaced] begin of line
                break;  // Ignore to EOL
            } else if (*pos == '/' && *(pos + 1) == '*') {
                inCmt = true;
                space_begin = false;
                // cppcheck-suppress StlMissingComparison
                ++pos;
            } else {
                if (!std::isspace(*pos)) space_begin = false;
                oline += *pos;
            }
        }
        whole_file += oline + " ";
    }
    whole_file += "\n";  // So string match below is simplified
    if (inCmt) fl->v3error("Unterminated /* comment inside -f file.");

    fl = new FileLine{filename};

    // Split into argument list and process
    // Note we try to respect escaped char, double/simple quoted strings
    // Other simulators don't respect a common syntax...

    // Strip off arguments and parse into words
    std::vector<string> args;

    // Parse file using a state machine, taking into account quoted strings and escaped chars
    enum state : uint8_t {
        ST_IN_OPTION,
        ST_ESCAPED_CHAR,
        ST_IN_QUOTED_STR,
        ST_IN_DOUBLE_QUOTED_STR
    };

    state st = ST_IN_OPTION;
    state last_st = ST_IN_OPTION;
    string arg;
    for (string::size_type pos = 0; pos < whole_file.length(); ++pos) {
        char curr_char = whole_file[pos];
        switch (st) {
        case ST_IN_OPTION:  // Get all chars up to a white space or a "="
            if (std::isspace(curr_char)) {  // End of option
                if (!arg.empty()) {  // End of word
                    args.push_back(arg);
                }
                arg = "";
                break;
            }
            if (curr_char == '\\') {  // Escape char, we wait for next char
                last_st = st;  // Memorize current state
                st = ST_ESCAPED_CHAR;
                break;
            }
            if (curr_char == '\'') {  // Find begin of quoted string
                // Examine next char in order to decide between
                // a string or a base specifier for integer literal
                ++pos;
                if (pos < whole_file.length()) curr_char = whole_file[pos];
                if (curr_char == '"') {  // String
                    st = ST_IN_QUOTED_STR;
                } else {  // Base specifier
                    arg += '\'';
                }
                arg += curr_char;
                break;
            }
            if (curr_char == '"') {  // Find begin of double quoted string
                // Doesn't insert the quote
                st = ST_IN_DOUBLE_QUOTED_STR;
                break;
            }
            arg += curr_char;
            break;
        case ST_IN_QUOTED_STR:  // Just store all chars inside string
            if (curr_char != '\'') {
                arg += curr_char;
            } else {  // End of quoted string
                st = ST_IN_OPTION;
            }
            break;
        case ST_IN_DOUBLE_QUOTED_STR:  // Take into account escaped chars
            if (curr_char != '"') {
                if (curr_char == '\\') {
                    last_st = st;
                    st = ST_ESCAPED_CHAR;
                } else {
                    arg += curr_char;
                }
            } else {  // End of double quoted string
                st = ST_IN_OPTION;
            }
            break;
        case ST_ESCAPED_CHAR:  // Just add the escaped char
            arg += curr_char;
            st = last_st;
            break;
        }
    }
    if (!arg.empty()) {  // Add last word
        args.push_back(arg);
    }

    // Path
    const string optdir = (rel ? V3Os::filenameDir(filename) : ".");

    // Convert to argv style arg list and parse them
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const string& i : args) argv.push_back(const_cast<char*>(i.c_str()));
    argv.push_back(nullptr);  // argv is nullptr-terminated
    parseOptsList(fl, optdir, static_cast<int>(argv.size() - 1), argv.data());
}

//======================================================================

string V3Options::parseFileArg(const string& optdir, const string& relfilename) {
    string filename = V3Os::filenameSubstitute(relfilename);
    if (optdir != "." && V3Os::filenameIsRel(filename))
        filename = V3Os::filenameJoin(optdir, filename);
    return filename;
}

//======================================================================

void V3Options::showVersion(bool verbose) {
    cout << version();
    cout << "\n";
    if (!verbose) return;

    cout << "\n";
    cout << "Copyright 2003-2025 by Wilson Snyder.  Verilator is free software; you can\n";
    cout << "redistribute it and/or modify the Verilator internals under the terms of\n";
    cout << "either the GNU Lesser General Public License Version 3 or the Perl Artistic\n";
    cout << "License Version 2.0.\n";

    cout << "\n";
    cout << "See https://verilator.org for documentation\n";

    cout << "\n";
    cout << "Summary of configuration:\n";
    cout << "  Compiled in defaults if not in environment:\n";
    cout << "    SYSTEMC            = " << DEFENV_SYSTEMC << "\n";
    cout << "    SYSTEMC_ARCH       = " << DEFENV_SYSTEMC_ARCH << "\n";
    cout << "    SYSTEMC_INCLUDE    = " << DEFENV_SYSTEMC_INCLUDE << "\n";
    cout << "    SYSTEMC_LIBDIR     = " << DEFENV_SYSTEMC_LIBDIR << "\n";
    cout << "    VERILATOR_ROOT     = " << DEFENV_VERILATOR_ROOT << "\n";
    cout << "    SystemC system-wide = " << cvtToStr(systemCSystemWide()) << "\n";

    // If update below, also update V3Options::getenvBuiltins()
    cout << "\n";
    cout << "Environment:\n";
    cout << "    MAKE               = " << V3Os::getenvStr("MAKE", "") << "\n";
    cout << "    PERL               = " << V3Os::getenvStr("PERL", "") << "\n";
    cout << "    PYTHON3            = " << V3Os::getenvStr("PYTHON3", "") << "\n";
    cout << "    SYSTEMC            = " << V3Os::getenvStr("SYSTEMC", "") << "\n";
    cout << "    SYSTEMC_ARCH       = " << V3Os::getenvStr("SYSTEMC_ARCH", "") << "\n";
    cout << "    SYSTEMC_INCLUDE    = " << V3Os::getenvStr("SYSTEMC_INCLUDE", "") << "\n";
    cout << "    SYSTEMC_LIBDIR     = " << V3Os::getenvStr("SYSTEMC_LIBDIR", "") << "\n";
    // wrapper uses VERILATOR_BIN
    cout << "    VERILATOR_BIN      = " << V3Os::getenvStr("VERILATOR_BIN", "") << "\n";
    cout << "    VERILATOR_ROOT     = " << V3Os::getenvStr("VERILATOR_ROOT", "") << "\n";

    // If update below, also update V3Options::getSupported()
    cout << "\n";
    cout << "Supported features (compiled-in or forced by environment):\n";
    cout << "    COROUTINES         = " << getSupported("COROUTINES") << "\n";
    cout << "    SYSTEMC            = " << getSupported("SYSTEMC") << "\n";
}

//======================================================================

V3Options::V3Options() {
    m_impp = new V3OptionsImp;

    m_traceFormat = TraceFormat::VCD;

    m_makeDir = "obj_dir";
    m_unusedRegexp = "*unused*";
    m_xAssign = "fast";
    m_xInitial = "unique";

    m_defaultLanguage = V3LangCode::mostRecent();

    VName::maxLength(128);  // Linux filename limits 256; leave half for prefix

    optimize(1);
    // Default +libext+
    addLibExtV("");  // So include "filename.v" will find the same file
    addLibExtV(".v");
    addLibExtV(".sv");
    // Default -I
    addIncDirFallback(".");  // Looks better than {long_cwd_path}/...
}

V3Options::~V3Options() { VL_DO_CLEAR(delete m_impp, m_impp = nullptr); }

void V3Options::setDebugMode(int level) {
    V3Error::debugDefault(level);
    if (!m_dumpLevel.count("tree")) m_dumpLevel["tree"] = 3;  // Don't override if already set.
    m_stats = true;
    m_debugCheck = true;
    cout << "Starting " << version() << "\n";
}

unsigned V3Options::debugLevel(const string& tag) const VL_MT_SAFE {
    const auto iter = m_debugLevel.find(tag);
    return iter != m_debugLevel.end() ? iter->second : V3Error::debugDefault();
}

unsigned V3Options::debugSrcLevel(const string& srcfile_path) const VL_MT_SAFE {
    // For simplicity, calling functions can just use __FILE__ for srcfile.
    // That means we need to strip the filenames: ../Foo.cpp -> Foo
    return debugLevel(V3Os::filenameNonDirExt(srcfile_path));
}

unsigned V3Options::dumpLevel(const string& tag) const VL_MT_SAFE {
    const auto iter = m_dumpLevel.find(tag);
    return iter != m_dumpLevel.end() ? iter->second : 0;
}

unsigned V3Options::dumpSrcLevel(const string& srcfile_path) const VL_MT_SAFE {
    // For simplicity, calling functions can just use __FILE__ for srcfile.
    // That means we need to strip the filenames: ../Foo.cpp -> Foo
    return dumpLevel(V3Os::filenameNonDirExt(srcfile_path));
}

bool V3Options::dumpTreeAddrids() const VL_MT_SAFE {
    static int level = -1;
    if (VL_UNLIKELY(level < 0)) {
        const unsigned value = dumpLevel("tree-addrids");
        if (!available()) return value > 0;
        level = static_cast<unsigned>(value);
    }
    return level > 0;
}

void V3Options::optimize(int level) {
    // Set all optimizations to on/off
    const bool flag = level > 0;
    m_fAcycSimp = flag;
    m_fAssemble = flag;
    m_fCase = flag;
    m_fCombine = flag;
    m_fConst = flag;
    m_fConstBitOpTree = flag;
    m_fDedupe = flag;
    m_fDfgPreInline = flag;
    m_fDfgPostInline = flag;
    m_fDfgScoped = flag;
    m_fDeadAssigns = flag;
    m_fDeadCells = flag;
    m_fExpand = flag;
    m_fGate = flag;
    m_fInline = flag;
    m_fLife = flag;
    m_fLifePost = flag;
    m_fLocalize = flag;
    m_fMergeCond = flag;
    m_fReloop = flag;
    m_fReorder = flag;
    m_fSplit = flag;
    m_fSubst = flag;
    m_fSubstConst = flag;
    m_fTable = flag;
    m_fVarSplit = flag;
    // And set specific optimization levels
    if (level >= 3) {
        m_inlineMult = -1;  // Maximum inlining
    }
}

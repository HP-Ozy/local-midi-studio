#pragma once
#include <juce_core/juce_core.h>

// Free sample libraries the app can download and install (all CC0 from the sfzinstruments GitHub organisation,
// served as ZIP archives that JUCE can extract), plus helpers to find what is installed in the library folder.
namespace Library
{
struct Item { const char* id; const char* name; const char* kind; const char* licence; int approxMb; const char* url; };

inline const std::vector<Item>& catalog()
{
    static const std::vector<Item> items {
        { "emilyguitar",    "Emilyguitar",        "acoustic guitar",   "CC0", 100, "https://github.com/sfzinstruments/karoryfer.emilyguitar/archive/HEAD.zip" },
        { "shinyguitar",    "Shinyguitar",        "archtop guitar",    "CC0", 360, "https://github.com/sfzinstruments/karoryfer.shinyguitar/archive/HEAD.zip" },
        { "weresax",        "Weresax",            "alto saxophone",    "CC0", 190, "https://github.com/sfzinstruments/karoryfer.weresax/archive/HEAD.zip" },
        { "bear-sax",       "Bear Sax",           "saxophone",         "CC0", 130, "https://github.com/sfzinstruments/karoryfer.bear-sax/archive/HEAD.zip" },
        { "cello",          "Bigcat Cello",       "cello",             "CC0", 130, "https://github.com/sfzinstruments/karoryfer-bigcat.cello/archive/HEAD.zip" },
        { "string-cyborgs", "String Cyborgs",     "experimental strings", "CC0", 60, "https://github.com/sfzinstruments/karoryfer.string-cyborgs/archive/HEAD.zip" },
        { "war-tuba",       "War Tuba",           "tuba",              "CC0", 105, "https://github.com/sfzinstruments/karoryfer.war-tuba/archive/HEAD.zip" },
        { "double-bass",    "D. Smolken Double Bass", "double bass",   "CC0", 260, "https://github.com/sfzinstruments/dsmolken.double-bass/archive/HEAD.zip" },
    };
    return items;
}

inline juce::File settingsFile() { return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("LocalMIDIStudio/library-folder.txt"); }

// Where instruments live: chosen by the user (saved), else Documents\LocalMIDIStudio\Instruments.
inline juce::File folder()
{
    const auto saved = settingsFile().loadFileAsString().trim();
    if (saved.isNotEmpty() && juce::File::isAbsolutePath (saved) && juce::File (saved).isDirectory()) return juce::File (saved);
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("LocalMIDIStudio/Instruments");
}

inline void setFolder (const juce::File& f) { settingsFile().getParentDirectory().createDirectory(); settingsFile().replaceWithText (f.getFullPathName()); }

// Installed instruments: every .sfz under the folder, sorted, grouped later by their top-level subfolder.
inline juce::Array<juce::File> installed (const juce::File& root)
{
    auto files = root.findChildFiles (juce::File::findFiles, true, "*.sfz");
    files.sort();
    return files;
}

// Background thread. Streams the archive to <root>/<id>.zip, extracts it into <root>/<id>/ and deletes the zip.
// JUCE's ZipFile refuses entries that would land outside the target folder. Returns an error message or "".
inline juce::String install (const juce::String& url, const juce::String& id, const juce::File& root,
                             const std::function<void (juce::int64)>& progress, const std::atomic<bool>& cancel)
{
    root.createDirectory();
    const auto zipFile = root.getChildFile (id + ".zip"), dest = root.getChildFile (id);
    juce::String error;
    {
        auto in = juce::URL (url).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                         .withConnectionTimeoutMs (20000).withNumRedirectsToFollow (5));
        if (in == nullptr) return "cannot connect to " + url;
        zipFile.deleteFile();
        juce::FileOutputStream out (zipFile);
        if (! out.openedOk()) return "cannot write " + zipFile.getFullPathName();
        juce::HeapBlock<char> buf (1 << 16);
        juce::int64 total = 0;
        for (int n; error.isEmpty() && (n = in->read (buf, 1 << 16)) > 0;)
        {
            if (cancel) error = "cancelled";
            else if (! out.write (buf, (size_t) n)) error = "disk full while writing " + zipFile.getFullPathName();
            else progress (total += n);
        }
        if (error.isEmpty() && total == 0) error = "empty download from " + url;
    } // the stream is closed here: only now can Windows delete a partial file
    if (error.isNotEmpty()) { zipFile.deleteFile(); return error; }
    juce::Result result = juce::Result::fail ("downloaded file is not a valid ZIP");
    {
        juce::ZipFile zip (zipFile); // closed at the end of this scope, so Windows lets us delete the archive
        if (zip.getNumEntries() > 0) result = zip.uncompressTo (dest, true);
    }
    zipFile.deleteFile();
    if (result.failed()) return "extraction failed: " + result.getErrorMessage();
    if (installed (dest).isEmpty()) return "no .sfz instrument inside the archive";
    return {};
}
} // namespace Library

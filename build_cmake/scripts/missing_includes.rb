#! /usr/bin/env ruby
#
# This script detects C++ header files that use common standard library classes like `std::vector`
# without including their corresponding headers (like `<vector>`). Such files may successfully
# compile with one compiler, or standard library implementation, but fail with another, due to
# differences in which other headers the standard library headers include.
#
# By Jens Alfke, Jan 2021

require "pathname"
require "set"

StdPrefix = "std::"

# ANSI terminal color escapes
PLAIN = "\033[0m"
BOLD = "\033[1m"
DIM = "\033[2m"
ITALIC = "\033[3m"
UNDERLINE = "\033[4m"
CYAN = "\033[96m"
YELLOW = "\033[93m"


# TODO: Some things are apparently defined in multiple headers, like std::swap in both algorithm and utility
# TODO: The official C++ library docs imply that each header has a known set of other headers it
#       includes; we could take advantage of that and reduce the number of false positives.

# Maps a regex for a class/fn name, to the name of the header it's defined in
Classes = Hash.new()

def look_for(header, classes = nil)
    classes = [header]  if classes == nil
    classes.each do |cls|
        Classes[Regexp.new("\\b" + StdPrefix + cls + "\\b")] = header
    end
end

look_for("algorithm",   ["binary_search", "clamp", "lower_bound", "max", "min", "minmax", "sort", "upper_bound"])
look_for("atomic",      ["atomic", "atomic_ref", "atomic_flag", "memory_order"])
look_for("chrono")
look_for("fstream",     ["filebuf", "ifstream", "ofstream", "fstream"])
look_for("functional",  ["function"])
look_for("initializer_list")
look_for("iostream",    ["cerr", "cin", "cout", "clog"])
look_for("map")
look_for("memory",      ["make_unique", "make_shared", "shared_ptr", "unique_ptr", "weak_ptr"])
look_for("mutex",       ["mutex", "timed_mutex", "recursive_mutex", "lock_guard", "unique_lock", "scoped_lock", "once_flag", "call_once"])
look_for("optional",    ["make_optional", "optional", "nullopt"])
look_for("set")
look_for("sstream",     ["string_stream"])
#look_for("string")         # Suppressed because it's so often omitted, and yet rarely causes problems
look_for("string_view")
look_for("tuple",       ["tie", "tuple"])
look_for("unordered_map")
look_for("unordered_set")
look_for("utility",     ["forward", "move", "pair", "get", "swap"])
look_for("variant",      ["variant", "visit", "get_if"])
look_for("vector")
# TODO: This is obviously incomplete. I've just been adding the most common stuff I find.


def scan_file(pathname)
    headers = Set.new()
    first = true
    lineno = 0
    file = File.new(pathname.to_s)
    file.each_line do |line|
        lineno += 1
        # TODO: Remove C-style comments, even multiline
        line = line.split("//")[0]

        if line =~ /\s*#include\s+<(\w+(\.h)?)>/ then
            # Found an #include<...>:
            headers.add($1)
        else
            Classes.each do |classRegexp, headerName|
                if not headers.include?(headerName) and line =~ classRegexp then
                    # Found a symbol without a prior #include of its header:
                    if first then
                        first = false
                        puts "#{BOLD}*** #{pathname.parent}/#{YELLOW}#{pathname.basename}#{PLAIN}"
                    end
                    name = classRegexp.source[2..-3]   # strip the "\b"
                    puts "\t\#include #{BOLD}#{CYAN}<#{headerName}>#{PLAIN}    \t#{ITALIC}#{DIM}// for #{name}, line #{lineno}#{PLAIN}"
                    headers.add(headerName)  # So I don't complain about the same header again
                    # TODO: Would be nice to alphabetize by header name
                end
            end
        end
    end
end


def scan_tree(dir)
    dir.find do |file|
        scan_file(file)  if file.extname == ".hh"
    end
end


if ARGV.empty? then
    root = Pathname.new($0).parent.parent.parent  # Assumes script is in build_cmake/scripts/
    puts "Scanning C++ headers in #{root.expand_path} ..."
    scan_tree(root)
else
    ARGV.each do |arg|
        scan_tree(Pathname.new(arg))
    end
end

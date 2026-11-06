# link_to_bin.cmake
# -----------------
# Runs after each build (POST_BUILD, "cmake -P"). It fills bin/<config>:
#  1) Symbolic links to the program, its PDB, the data folder, the Xerces-C DLL
#     and 7-Zip. The links always point to the newest build output.
#  2) windeployqt puts the Qt DLLs and the Qt plugins in bin/<config>.
#
# The program file is a HARD link, not a symbolic link: ShellExecute (a
# double-click in Explorer) starts the TARGET of a symbolic link. Then Windows
# looks for the DLLs in the build folder and the program does not start.
# A hard link is a second name of the same file, so Windows starts it from
# bin/<config> and finds the Qt DLLs there. All other items are symbolic links.
#
# If Windows does not let the script make a symbolic link (Developer Mode is
# off and the user is not an administrator), the script copies the item and
# writes a warning.
#
# Input variables (-D): TE_CONFIG, TE_EXE, TE_PDB, TE_BIN_DIR, TE_ROOT,
#                       TE_COTS, TE_XERCES_DLL

cmake_minimum_required(VERSION 3.16)

file(MAKE_DIRECTORY "${TE_BIN_DIR}")

# Removes a file, a folder or a link. For a directory link, only the link is removed.
function(te_remove_entry path)
	if(IS_SYMLINK "${path}")
		if(IS_DIRECTORY "${path}")
			file(TO_NATIVE_PATH "${path}" _native)
			execute_process(COMMAND cmd /c rmdir "${_native}")
		else()
			file(REMOVE "${path}")
		endif()
	elseif(IS_DIRECTORY "${path}")
		file(REMOVE_RECURSE "${path}")
	elseif(EXISTS "${path}")
		file(REMOVE "${path}")
	endif()
endfunction()

function(te_same_path a b out)
	file(TO_CMAKE_PATH "${a}" _a)
	file(TO_CMAKE_PATH "${b}" _b)
	string(REGEX REPLACE "^//\\?/" "" _a "${_a}")
	string(REGEX REPLACE "^//\\?/" "" _b "${_b}")
	string(TOLOWER "${_a}" _a)
	string(TOLOWER "${_b}" _b)
	if(_a STREQUAL _b)
		set(${out} TRUE PARENT_SCOPE)
	else()
		set(${out} FALSE PARENT_SCOPE)
	endif()
endfunction()

# Makes the symbolic link dst -> src. Does nothing if the link is already correct.
function(te_link src dst)
	if(NOT EXISTS "${src}")
		return()
	endif()
	if(IS_SYMLINK "${dst}")
		file(READ_SYMLINK "${dst}" _cur)
		te_same_path("${_cur}" "${src}" _same)
		if(_same)
			return()
		endif()
	endif()
	te_remove_entry("${dst}")
	file(CREATE_LINK "${src}" "${dst}" RESULT _res SYMBOLIC)
	if(NOT _res EQUAL 0)
		message(WARNING "Cannot make a symbolic link (${_res}): ${dst}. The item is copied. Enable Windows Developer Mode to use links.")
		if(IS_DIRECTORY "${src}")
			file(COPY "${src}/" DESTINATION "${dst}")
		else()
			configure_file("${src}" "${dst}" COPYONLY)
		endif()
	endif()
endfunction()

# Makes the hard link dst -> src (same file, same volume). If dst is already
# the same file (same size and time), the function does nothing.
function(te_hard_link src dst)
	if(NOT EXISTS "${src}")
		return()
	endif()
	if(EXISTS "${dst}" AND NOT IS_SYMLINK "${dst}")
		file(SIZE "${src}" _s1)
		file(SIZE "${dst}" _s2)
		file(TIMESTAMP "${src}" _t1 "%Y%m%d%H%M%S" UTC)
		file(TIMESTAMP "${dst}" _t2 "%Y%m%d%H%M%S" UTC)
		if(_s1 EQUAL _s2 AND _t1 STREQUAL _t2)
			return()
		endif()
	endif()
	te_remove_entry("${dst}")
	file(CREATE_LINK "${src}" "${dst}" RESULT _res)
	if(NOT _res EQUAL 0)
		message(WARNING "Cannot make a hard link (${_res}): ${dst}. The file is copied.")
		configure_file("${src}" "${dst}" COPYONLY)
	endif()
endfunction()

get_filename_component(_exe_name "${TE_EXE}" NAME)
get_filename_component(_pdb_name "${TE_PDB}" NAME)

te_hard_link("${TE_EXE}" "${TE_BIN_DIR}/${_exe_name}")
if(TE_PDB)
	te_link("${TE_PDB}" "${TE_BIN_DIR}/${_pdb_name}")
endif()
te_link("${TE_ROOT}/data" "${TE_BIN_DIR}/data")

if(TE_XERCES_DLL)
	get_filename_component(_x_name "${TE_XERCES_DLL}" NAME)
	te_link("${TE_XERCES_DLL}" "${TE_BIN_DIR}/${_x_name}")
endif()

foreach(_f 7z.exe 7z.dll 7za.exe 7zr.exe)
	te_link("${TE_COTS}/7zip/${_f}" "${TE_BIN_DIR}/${_f}")
endforeach()

# --- windeployqt: Qt DLLs and plugins -> bin/<config> ---
set(_wdq "${TE_COTS}/qt5/bin/windeployqt.exe")
if(NOT EXISTS "${_wdq}")
	message(FATAL_ERROR "windeployqt is not found: ${_wdq}. Run build/setup_cots.bat.")
endif()
string(TOLOWER "${TE_CONFIG}" _cfg)
if(_cfg STREQUAL "debug")
	set(_mode --debug)
else()
	set(_mode --release)
endif()
execute_process(
	COMMAND "${_wdq}" ${_mode} --no-translations --no-angle --no-opengl-sw
		--no-system-d3d-compiler --no-compiler-runtime
		--dir "${TE_BIN_DIR}" "${TE_BIN_DIR}/${_exe_name}"
	RESULT_VARIABLE _r
	OUTPUT_VARIABLE _o
	ERROR_VARIABLE _e)
if(NOT _r EQUAL 0)
	message(FATAL_ERROR "windeployqt failed (${_r}):\n${_o}\n${_e}")
endif()
message(STATUS "bin/${TE_CONFIG} is ready: ${TE_BIN_DIR}/${_exe_name}")

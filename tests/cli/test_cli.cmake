set(valid "${WORK_DIR}/tavl_cli_valid.tavl")
set(bad_string "${WORK_DIR}/tavl_cli_bad_string.tavl")
set(bad_utf8 "${WORK_DIR}/tavl_cli_bad_utf8.tavl")
set(bad_comment "${WORK_DIR}/tavl_cli_bad_comment.tavl")
set(unknown "${WORK_DIR}/tavl_cli_unknown.tavl")
set(duplicate "${WORK_DIR}/tavl_cli_duplicate.tavl")
set(comments "${WORK_DIR}/tavl_cli_comments.tavl")
set(logs "${WORK_DIR}/tavl_cli_logs.tavl")
set(merge_left "${WORK_DIR}/tavl_cli_merge_left.tavl")
set(merge_right "${WORK_DIR}/tavl_cli_merge_right.tavl")

file(WRITE "${valid}" [=[
b = 10 20 30
c = (10, 20, 30)
d = [10, 20, 30]
e = [10 20 30]
f = (10 20 30)
window = { width = 1920 }
]=])
file(WRITE "${bad_string}" "x = \"broken")
string(ASCII 255 bad_utf8_byte)
file(WRITE "${bad_utf8}" "x=\"${bad_utf8_byte}\"\n")
string(ASCII 27 raw_escape_byte)
set(bad_control "${WORK_DIR}/tavl_cli_bad_control.tavl")
file(WRITE "${bad_control}" "x=\"${raw_escape_byte}\"\n")
set(bad_escape "${WORK_DIR}/tavl_cli_bad_escape.tavl")
file(WRITE "${bad_escape}" [=[x="\q"
]=])
file(WRITE "${bad_comment}" "x = 1 /* broken")
file(WRITE "${unknown}" "x = word@word")
file(WRITE "${duplicate}" "x = 1\nx = 2\n")
set(duplicate_object "${WORK_DIR}/tavl_cli_duplicate_object.tavl")
file(WRITE "${duplicate_object}" "x={a=1,\"a\"=2}\n")
set(duplicate_tuple "${WORK_DIR}/tavl_cli_duplicate_tuple.tavl")
file(WRITE "${duplicate_tuple}" "x=(a=1,a=2)\n")
set(missing_value "${WORK_DIR}/tavl_cli_missing_value.tavl")
file(WRITE "${missing_value}" "a=\n")
set(array_operator "${WORK_DIR}/tavl_cli_array_operator.tavl")
file(WRITE "${array_operator}" "a=[a =]\n")
set(missing_identifier "${WORK_DIR}/tavl_cli_missing_identifier.tavl")
file(WRITE "${missing_identifier}" "x=(= =)\n")
set(object_value "${WORK_DIR}/tavl_cli_object_value.tavl")
file(WRITE "${object_value}" "x={value}\n")
set(single_value "${WORK_DIR}/tavl_cli_single_value.tavl")
file(WRITE "${single_value}" "a=1\n")
file(WRITE "${comments}" "x = { a = 1 // keep this\n}\ny = 2\n")
file(WRITE "${logs}" "1999-01-01_06:54:32 { abc = fail, reason = io_error }\n1999-01-01_06:54:33 { abc = ok, value = 123 }\n")
file(WRITE "${merge_left}" "settings = { x = 1, keep = yes, nested = { a = 1, list = [1] } }\nitems = [1, 2]\nold = 7\n")
file(WRITE "${merge_right}" "settings = { x = 2, add = 3, nested = { a = 2, b = 3, list = [2] } }\nitems = [3]\nnew = 4\n")

execute_process(COMMAND "${CLI}" check "${valid}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT err STREQUAL "")
  message(FATAL_ERROR "check valid: rc=${rc}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" check "${missing_value}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "err_missing_value")
  message(FATAL_ERROR "missing assignment value: rc=${rc}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" check "${missing_identifier}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "err_expected_identifier")
  message(FATAL_ERROR "missing assignment identifier: rc=${rc}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" check "${object_value}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT err MATCHES "warning: warn_object_without_value")
  message(FATAL_ERROR "unkeyed object value is a warning: rc=${rc}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" check "${CMAKE_CURRENT_LIST_DIR}/../format/diagnostics.tavl"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "warn_object_without_value" OR
   NOT err MATCHES "err_expected_identifier" OR
   NOT err MATCHES "err_missing_value" OR
   NOT err MATCHES "err_invalid_escape" OR
   NOT err MATCHES "err_float_out_of_range" OR
   NOT err MATCHES "err_empty_row" OR
   NOT err MATCHES "err_duplicate_field" OR
   NOT err MATCHES "err_bracket_missmatch" OR
   NOT err MATCHES "err_unterminated_block_comment")
  message(FATAL_ERROR "diagnostic example: rc=${rc}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" check "${CMAKE_CURRENT_LIST_DIR}/../format/unterminated_string.tavl"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "err_unterminated_string")
  message(FATAL_ERROR "unterminated string example: rc=${rc}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" check "${bad_utf8}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "err_invalid_utf8")
  message(FATAL_ERROR "ill-formed UTF-8: rc=${rc}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" check "${bad_control}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "err_forbidden_control")
  message(FATAL_ERROR "literal ESC: rc=${rc}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" check "${array_operator}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT err STREQUAL "")
  message(FATAL_ERROR "array tokens are data: rc=${rc}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" check "${duplicate}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT err STREQUAL "")
  message(FATAL_ERROR "repeated root rows remain valid: rc=${rc}, stderr=${err}")
endif()
foreach(duplicate_file IN ITEMS "${duplicate_object}" "${duplicate_tuple}")
  execute_process(COMMAND "${CLI}" check "${duplicate_file}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 2 OR NOT err MATCHES "error: err_duplicate_field")
    message(FATAL_ERROR "duplicate named field: rc=${rc}, stderr=${err}")
  endif()
  execute_process(COMMAND "${CLI}" get ".x" "${duplicate_file}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 2 OR NOT out STREQUAL "")
    message(FATAL_ERROR "get must fail on duplicate named field: rc=${rc}, stdout=${out}")
  endif()
endforeach()

function(expect_get path expected)
  execute_process(COMMAND "${CLI}" get "${path}" "${valid}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0 OR NOT out STREQUAL "${expected}\n" OR NOT err STREQUAL "")
    message(FATAL_ERROR "get ${path}: rc=${rc}, stdout=${out}, stderr=${err}")
  endif()
endfunction()

expect_get(".b" "10 20 30")
expect_get(".b[1]" "20")
expect_get(".c[1]" "20")
expect_get(".d[1]" "20")
expect_get(".e[0]" "10 20 30")
expect_get(".e[0][1]" "20")
expect_get(".f[1]" "20")
expect_get(".window.width" "1920")

set(spaced_array "${WORK_DIR}/tavl_cli_spaced_array.tavl")
set(comma_array "${WORK_DIR}/tavl_cli_comma_array.tavl")
file(WRITE "${spaced_array}" "matrix=[[1 2],[3 4]]\n")
file(WRITE "${comma_array}" "matrix=[[1,2],[3,4]]\n")
execute_process(COMMAND "${CLI}" get ".matrix[0][0]" "${spaced_array}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "1 2\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "space-separated array row: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" get ".matrix[0][1]" "${spaced_array}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 3 OR NOT out STREQUAL "")
  message(FATAL_ERROR "space-separated array has one row: rc=${rc}, stdout=${out}")
endif()
execute_process(COMMAND "${CLI}" get ".matrix[0][1]" "${comma_array}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "2\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "comma-separated array has two rows: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
set(row_array "${WORK_DIR}/tavl_cli_row_array.tavl")
file(WRITE "${row_array}" "items=[1 2,3]\n")
execute_process(COMMAND "${CLI}" insert ".items" 1 8 "${row_array}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE changed ERROR_VARIABLE err)
file(WRITE "${WORK_DIR}/tavl_cli_row_inserted.tavl" "${changed}")
execute_process(COMMAND "${CLI}" get ".items[1]" "${WORK_DIR}/tavl_cli_row_inserted.tavl"
  RESULT_VARIABLE read_rc OUTPUT_VARIABLE out ERROR_VARIABLE read_err)
if(NOT rc EQUAL 0 OR NOT read_rc EQUAL 0 OR NOT out STREQUAL "8\n" OR
   NOT changed MATCHES "items=\\[1 2,8, 3\\]" OR NOT err STREQUAL "")
  message(FATAL_ERROR "insert at array row boundary: rc=${rc}, stdout=${changed}, read=${out}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" set ".window" "{ width = 1280, height = 720 }" "${valid}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE changed ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT err STREQUAL "")
  message(FATAL_ERROR "set block: rc=${rc}, stderr=${err}")
endif()
file(WRITE "${WORK_DIR}/tavl_cli_changed.tavl" "${changed}")
execute_process(COMMAND "${CLI}" get ".window.height" "${WORK_DIR}/tavl_cli_changed.tavl"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "720\n")
  message(FATAL_ERROR "read replaced block: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" set ".b[1]" "99" "${valid}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE changed ERROR_VARIABLE err)
file(WRITE "${WORK_DIR}/tavl_cli_changed.tavl" "${changed}")
execute_process(COMMAND "${CLI}" get ".b[1]" "${WORK_DIR}/tavl_cli_changed.tavl"
  RESULT_VARIABLE read_rc OUTPUT_VARIABLE out ERROR_VARIABLE read_err)
if(NOT rc EQUAL 0 OR NOT read_rc EQUAL 0 OR NOT out STREQUAL "99\n")
  message(FATAL_ERROR "set inline element: rc=${rc}, read_rc=${read_rc}, stdout=${out}")
endif()

execute_process(COMMAND "${CLI}" append ".window" "height = 720" "${valid}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE changed ERROR_VARIABLE err)
file(WRITE "${WORK_DIR}/tavl_cli_changed.tavl" "${changed}")
execute_process(COMMAND "${CLI}" get ".window.height" "${WORK_DIR}/tavl_cli_changed.tavl"
  RESULT_VARIABLE read_rc OUTPUT_VARIABLE out ERROR_VARIABLE read_err)
if(NOT rc EQUAL 0 OR NOT read_rc EQUAL 0 OR NOT out STREQUAL "720\n")
  message(FATAL_ERROR "append object row: rc=${rc}, read_rc=${read_rc}, stdout=${out}")
endif()

execute_process(COMMAND "${CLI}" append ".d" "40" "${valid}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE changed ERROR_VARIABLE err)
file(WRITE "${WORK_DIR}/tavl_cli_changed.tavl" "${changed}")
execute_process(COMMAND "${CLI}" get ".d[3]" "${WORK_DIR}/tavl_cli_changed.tavl"
  RESULT_VARIABLE read_rc OUTPUT_VARIABLE out ERROR_VARIABLE read_err)
if(NOT rc EQUAL 0 OR NOT read_rc EQUAL 0 OR NOT out STREQUAL "40\n")
  message(FATAL_ERROR "append array element: rc=${rc}, read_rc=${read_rc}, stdout=${out}")
endif()

execute_process(COMMAND "${CLI}" append ".window.width" "height = 720" "${valid}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT out STREQUAL "")
  message(FATAL_ERROR "append to scalar: rc=${rc}, stdout=${out}")
endif()

execute_process(COMMAND "${CLI}" append ".x" "b = 3" "${comments}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE changed ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT changed MATCHES "// keep this" OR
   NOT changed MATCHES "y = 2")
  message(FATAL_ERROR "append must retain surrounding text: rc=${rc}, stdout=${changed}")
endif()
file(WRITE "${WORK_DIR}/tavl_cli_changed.tavl" "${changed}")
execute_process(COMMAND "${CLI}" get ".x.b" "${WORK_DIR}/tavl_cli_changed.tavl"
  RESULT_VARIABLE read_rc OUTPUT_VARIABLE out ERROR_VARIABLE read_err)
if(NOT read_rc EQUAL 0 OR NOT out STREQUAL "3\n")
  message(FATAL_ERROR "append after line comment: rc=${read_rc}, stdout=${out}")
endif()

execute_process(COMMAND "${CLI}" set ".x" "3" "${duplicate}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 4 OR NOT out STREQUAL "")
  message(FATAL_ERROR "ambiguous edit: rc=${rc}, stdout=${out}")
endif()

execute_process(COMMAND "${CLI}" del ".window.width" "${valid}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE changed ERROR_VARIABLE err)
file(WRITE "${WORK_DIR}/tavl_cli_changed.tavl" "${changed}")
execute_process(COMMAND "${CLI}" get ".window.width" "${WORK_DIR}/tavl_cli_changed.tavl"
  RESULT_VARIABLE read_rc OUTPUT_VARIABLE out ERROR_VARIABLE read_err)
if(NOT rc EQUAL 0 OR NOT read_rc EQUAL 3 OR changed MATCHES "width = 1920")
  message(FATAL_ERROR "delete named row: rc=${rc}, read_rc=${read_rc}, stdout=${changed}")
endif()

execute_process(COMMAND "${CLI}" del ".b" "${valid}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE changed ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR changed MATCHES "b = 10 20 30" OR
   NOT changed MATCHES "c = \\(10, 20, 30\\)")
  message(FATAL_ERROR "delete top-level row: rc=${rc}, stdout=${changed}")
endif()

execute_process(COMMAND "${CLI}" del ".d[2]" "${valid}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE changed ERROR_VARIABLE err)
file(WRITE "${WORK_DIR}/tavl_cli_changed.tavl" "${changed}")
execute_process(COMMAND "${CLI}" get ".d[1]" "${WORK_DIR}/tavl_cli_changed.tavl"
  RESULT_VARIABLE read_rc OUTPUT_VARIABLE out ERROR_VARIABLE read_err)
if(NOT rc EQUAL 0 OR NOT read_rc EQUAL 0 OR NOT out STREQUAL "20\n")
  message(FATAL_ERROR "delete last tuple item: rc=${rc}, read_rc=${read_rc}, stdout=${changed}")
endif()

execute_process(COMMAND "${CLI}" del ".b[1]" "${valid}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE changed ERROR_VARIABLE err)
file(WRITE "${WORK_DIR}/tavl_cli_changed.tavl" "${changed}")
execute_process(COMMAND "${CLI}" get ".b[1]" "${WORK_DIR}/tavl_cli_changed.tavl"
  RESULT_VARIABLE read_rc OUTPUT_VARIABLE out ERROR_VARIABLE read_err)
if(NOT rc EQUAL 0 OR NOT read_rc EQUAL 0 OR NOT out STREQUAL "30\n")
  message(FATAL_ERROR "delete inline positional item: rc=${rc}, read_rc=${read_rc}, stdout=${changed}")
endif()

execute_process(COMMAND "${CLI}" del ".a[0]" "${single_value}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE changed ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT changed STREQUAL "a=null\n")
  message(FATAL_ERROR "delete last named value: rc=${rc}, stdout=${changed}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" eval "del(@data[0])" "${single_value}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE changed ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT changed STREQUAL "a=null\n")
  message(FATAL_ERROR "eval delete last named value: rc=${rc}, stdout=${changed}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" del ".x" "${duplicate}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 4 OR NOT out STREQUAL "")
  message(FATAL_ERROR "ambiguous delete: rc=${rc}, stdout=${out}")
endif()

execute_process(COMMAND "${CLI}" filter ".abc" "ok" "${logs}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "1999-01-01_06:54:33 { abc = ok, value = 123 }\n" OR
   NOT err STREQUAL "")
  message(FATAL_ERROR "stream filter: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" filter ".reason" "io_error" "${logs}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "abc = fail")
  message(FATAL_ERROR "filter nested field: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" select ".abc" "${logs}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "fail\nok\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "stream select: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" map ".abc" "ready" "${logs}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "abc = ready, reason" OR
   NOT out MATCHES "abc = ready, value")
  message(FATAL_ERROR "stream map: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

file(WRITE "${WORK_DIR}/tavl_cli_trivia.tavl" "x = 1\n// trailing comment")
execute_process(COMMAND "${CLI}" map "[0]" "2" "${WORK_DIR}/tavl_cli_trivia.tavl"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "x = 2\n// trailing comment")
  message(FATAL_ERROR "map preserves trailing comment: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

file(WRITE "${WORK_DIR}/tavl_cli_comma.tavl" "a = { x = ok }, b = { x = fail }")
execute_process(COMMAND "${CLI}" filter ".x" "ok" "${WORK_DIR}/tavl_cli_comma.tavl"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "a = { x = ok }\n")
  message(FATAL_ERROR "filter comma-separated rows: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

string(REPEAT "1999-01-01_06:54:32 { abc = fail }\n" 2500 bulk)
file(WRITE "${WORK_DIR}/tavl_cli_bulk.tavl" "${bulk}1999-01-01_06:54:33 { abc = ok }\n")
execute_process(COMMAND "${CLI}" filter ".abc" "ok" "${WORK_DIR}/tavl_cli_bulk.tavl"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "1999-01-01_06:54:33 { abc = ok }\n")
  message(FATAL_ERROR "filter across input chunks: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" merge "${merge_left}" "${merge_right}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE changed ERROR_VARIABLE err)
file(WRITE "${WORK_DIR}/tavl_cli_changed.tavl" "${changed}")
if(NOT rc EQUAL 0 OR NOT err STREQUAL "")
  message(FATAL_ERROR "merge: rc=${rc}, stderr=${err}")
endif()
foreach(case IN ITEMS "settings.x=2" "settings.keep=yes" "settings.add=3" "settings.nested.a=2" "settings.nested.b=3" "settings.nested.list[0]=2" "items[0]=3" "old=7" "new=4")
  string(REPLACE "=" ";" parts "${case}")
  list(GET parts 0 key)
  list(GET parts 1 expected)
  execute_process(COMMAND "${CLI}" get ".${key}" "${WORK_DIR}/tavl_cli_changed.tavl"
    RESULT_VARIABLE read_rc OUTPUT_VARIABLE out ERROR_VARIABLE read_err)
  if(NOT read_rc EQUAL 0 OR NOT out STREQUAL "${expected}\n")
    message(FATAL_ERROR "merge field ${key}: rc=${read_rc}, stdout=${out}, stderr=${read_err}")
  endif()
endforeach()

execute_process(COMMAND "${CLI}" merge --arrays=append "${merge_left}" "${merge_right}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE changed ERROR_VARIABLE err)
file(WRITE "${WORK_DIR}/tavl_cli_changed.tavl" "${changed}")
execute_process(COMMAND "${CLI}" get ".items[2]" "${WORK_DIR}/tavl_cli_changed.tavl"
  RESULT_VARIABLE read_rc OUTPUT_VARIABLE out ERROR_VARIABLE read_err)
if(NOT rc EQUAL 0 OR NOT read_rc EQUAL 0 OR NOT out STREQUAL "3\n")
  message(FATAL_ERROR "merge append arrays: rc=${rc}, read_rc=${read_rc}, stdout=${changed}")
endif()
execute_process(COMMAND "${CLI}" get ".settings.nested.list[1]" "${WORK_DIR}/tavl_cli_changed.tavl"
  RESULT_VARIABLE read_rc OUTPUT_VARIABLE out ERROR_VARIABLE read_err)
if(NOT read_rc EQUAL 0 OR NOT out STREQUAL "2\n")
  message(FATAL_ERROR "merge append nested array: rc=${read_rc}, stdout=${changed}")
endif()

execute_process(COMMAND "${CLI}" merge "${duplicate}" "${merge_right}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 4 OR NOT out STREQUAL "" OR NOT err MATCHES "duplicate name")
  message(FATAL_ERROR "merge duplicate rejection: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

file(WRITE "${WORK_DIR}/tavl_cli_positional.tavl" "(ok, 1999-01-01_06:54:32)\n")
execute_process(COMMAND "${CLI}" merge "${WORK_DIR}/tavl_cli_positional.tavl" "${merge_right}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 4 OR NOT out STREQUAL "" OR NOT err MATCHES "requires named rows")
  message(FATAL_ERROR "merge positional rejection: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

set(eval_logs "${WORK_DIR}/tavl_cli_eval_logs.tavl")
file(WRITE "${eval_logs}" "1999-01-01_03:00:00 { abc = fail, reason = io_error }\n1999-01-01_06:00:00 { abc = fail, reason = io_error, def = { x = bad } }\n1999-01-01_07:00:00 { abc = fail, reason = io_error }\n2000-01-01_06:00:00 { abc = fail }\n")
execute_process(COMMAND "${CLI}" eval "where(date(@name) == 1999-01-01) | where(time(@name) > 04:04:04) | edit(@data[0]) { set(.abc, reviewed), del(.reason), where(exist(.def)) | edit(.def[0]) { set(.x, ok) } }" "${eval_logs}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(expected "1999-01-01_06:00:00 { abc = reviewed, def = { x = ok } }\n1999-01-01_07:00:00 { abc = reviewed }\n")
if(NOT rc EQUAL 0 OR NOT out STREQUAL expected OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval pipeline: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" eval "where(.abc == \"fail\")" "${eval_logs}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "1999-01-01_03:00:00 { abc = fail, reason = io_error }\n1999-01-01_06:00:00 { abc = fail, reason = io_error, def = { x = bad } }\n1999-01-01_07:00:00 { abc = fail, reason = io_error }\n2000-01-01_06:00:00 { abc = fail }\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval quoted scalar: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" eval "edit(@data[0]) { where(exist(.def)) | set(.abc, wrong), set(.abc, done) }" "${eval_logs}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "1999-01-01_03:00:00 \\{ abc = done, reason = io_error \\}" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval comma steps: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" eval "where(date(@name) == 1900-01-01), where(date(@name) == 1999-01-01)" "${eval_logs}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "1999-01-01_03:00:00" OR out MATCHES "2000-01-01" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval root comma steps: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" eval "where(date(@name) ==)" "${eval_logs}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT out STREQUAL "")
  message(FATAL_ERROR "eval malformed query: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

set(nested_arrays "${WORK_DIR}/tavl_cli_nested_arrays.tavl")
execute_process(COMMAND "${CLI}" eval "where(count(@data[0][0][*]) == 1)" "${spaced_array}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "matrix=[[1 2],[3 4]]\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval count space-separated array rows: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" eval "where(count(@data[0][0][*]) == 2)" "${comma_array}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "matrix=[[1,2],[3,4]]\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval count comma-separated array rows: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
set(indexed_array "${WORK_DIR}/tavl_cli_indexed_array.tavl")
file(WRITE "${indexed_array}" "items=[{x=1},{x=2}]\n")
execute_process(COMMAND "${CLI}" eval "edit(@data[0][*]) { where(@name == 1) | set(.x, 9) }" "${indexed_array}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "items=[{x=1},{x=9}]\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "array row @name index: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" eval "where(any(@data[0][*], @name == 2))" "${indexed_array}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "" OR NOT err STREQUAL "")
  message(FATAL_ERROR "array index outside row count: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
file(WRITE "${nested_arrays}" "matrix = [[1],[2],[3]]\n")
execute_process(COMMAND "${CLI}" eval "append(@data[0][*], 4)" "${nested_arrays}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "matrix = [[1, 4],[2, 4],[3, 4]]\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval append to every nested array: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
file(WRITE "${WORK_DIR}/tavl_cli_eval_changed.tavl" "${out}")
execute_process(COMMAND "${CLI}" check "${WORK_DIR}/tavl_cli_eval_changed.tavl"
  RESULT_VARIABLE check_rc OUTPUT_VARIABLE check_out ERROR_VARIABLE check_err)
if(NOT check_rc EQUAL 0 OR NOT check_err STREQUAL "")
  message(FATAL_ERROR "eval append output invalid: rc=${check_rc}, stderr=${check_err}")
endif()

execute_process(COMMAND "${CLI}" eval "set(@data[0][*][0], 9)" "${nested_arrays}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "matrix = [[9],[9],[9]]\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval wildcard set: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" eval "del(@data[0][*][0])" "${nested_arrays}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "matrix = [[],[],[]]\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval wildcard del: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" eval "del(@data[0][*])" "${nested_arrays}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "matrix = []\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval wildcard del adjacent rows: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
file(WRITE "${nested_arrays}" "matrix = [[[1],[2]],[[3]]]\n")
execute_process(COMMAND "${CLI}" eval "append(@data[0][*][*], 4)" "${nested_arrays}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "matrix = [[[1, 4],[2, 4]],[[3, 4]]]\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval two wildcard levels: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

set(nested_tuple "${WORK_DIR}/tavl_cli_nested_tuple.tavl")
file(WRITE "${nested_tuple}" "matrix = ([1], [2])\n")
execute_process(COMMAND "${CLI}" eval "edit(@data[0]) { edit([*]) { append(., 4) } }" "${nested_tuple}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "matrix = ([1, 4], [2, 4])\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval wildcard tuple edit: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

set(nested_object "${WORK_DIR}/tavl_cli_nested_object.tavl")
file(WRITE "${nested_object}" "matrix = { a = { x = 1 }, b = { x = 2 } }\n")
execute_process(COMMAND "${CLI}" eval "edit(@data[0]) { edit([*]) { set(.x, 9), append(., y = 4) } }" "${nested_object}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "a = \\{ x = 9" OR NOT out MATCHES "b = \\{ x = 9" OR NOT out MATCHES "y = 4" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval wildcard object edit: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
file(WRITE "${WORK_DIR}/tavl_cli_eval_changed.tavl" "${out}")
execute_process(COMMAND "${CLI}" check "${WORK_DIR}/tavl_cli_eval_changed.tavl"
  RESULT_VARIABLE check_rc OUTPUT_VARIABLE check_out ERROR_VARIABLE check_err)
if(NOT check_rc EQUAL 0 OR NOT check_err STREQUAL "")
  message(FATAL_ERROR "eval wildcard object output invalid: rc=${check_rc}, stderr=${check_err}")
endif()
file(WRITE "${nested_object}" "matrix = { a = [1], b = [2] }\n")
execute_process(COMMAND "${CLI}" eval "edit(@data[0][*]) { where(@name == a) | append(., 4) }" "${nested_object}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "matrix = { a = [1, 4], b = [2] }\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval wildcard row name: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

set(aggregate "${WORK_DIR}/tavl_cli_aggregate.tavl")
file(WRITE "${aggregate}" "groups = [{ score = 1 }, { score = 10 }, { score = 3 }]\n")
foreach(program IN ITEMS
    "where(count(@data[0][*]) == 3)"
    "where(count(@data[0][*]) > 2)"
    "where(any(@data[0][*], .score > 9))"
    "where(all(@data[0][*], .score > 0))")
  execute_process(COMMAND "${CLI}" eval "${program}" "${aggregate}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0 OR NOT out STREQUAL "groups = [{ score = 1 }, { score = 10 }, { score = 3 }]\n" OR NOT err STREQUAL "")
    message(FATAL_ERROR "eval aggregate ${program}: rc=${rc}, stdout=${out}, stderr=${err}")
  endif()
endforeach()
foreach(program IN ITEMS
    "where(count(@data[0][*]) > 9)"
    "where(all(@data[0][*], .score > 2))"
    "where(any(@data[0][*], .score > 10))")
  execute_process(COMMAND "${CLI}" eval "${program}" "${aggregate}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0 OR NOT out STREQUAL "" OR NOT err STREQUAL "")
    message(FATAL_ERROR "eval aggregate false ${program}: rc=${rc}, stdout=${out}, stderr=${err}")
  endif()
endforeach()
file(WRITE "${aggregate}" "groups = []\n")
execute_process(COMMAND "${CLI}" eval "where(all(@data[0][*], .score > 0) == true) | where(count(@data[0][*]) == 0)" "${aggregate}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "groups = []\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval empty all/count: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" eval "where(any(@data[0][*], .score > 0))" "${aggregate}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval empty any: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
file(WRITE "${aggregate}" "numbers = [1,2,3,4,5,6,7,8,9,10]\n")
execute_process(COMMAND "${CLI}" eval "where(count(@data[0][*]) > 9) | where(@data[0][9] > 2)" "${aggregate}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "numbers = [1,2,3,4,5,6,7,8,9,10]\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval numeric rather than lexical comparison: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

set(data_sequence "${WORK_DIR}/tavl_cli_data_sequence.tavl")
file(WRITE "${data_sequence}" "b = 10 20\n")
execute_process(COMMAND "${CLI}" eval "append(@data, 30)" "${data_sequence}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "b = 10 20 30\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval append row data: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" eval "insert(@data, 1, 15)" "${data_sequence}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "b = 10 15 20\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval insert row data: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
file(WRITE "${data_sequence}" "b = (10,20)\n")
execute_process(COMMAND "${CLI}" eval "append(@data, 30)" "${data_sequence}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "b = (10,20) 30\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval row data distinct from nested block: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

file(WRITE "${nested_arrays}" "matrix = [[1,3],[4,6]]\n")
execute_process(COMMAND "${CLI}" eval "insert(@data[0][*], 1, 2)" "${nested_arrays}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "matrix = [[1,2, 3],[4,2, 6]]\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval wildcard insert: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
file(WRITE "${nested_arrays}" "items = [1 2 3]\n")
execute_process(COMMAND "${CLI}" eval "insert(@data[0], 1, 9)" "${nested_arrays}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
file(WRITE "${WORK_DIR}/tavl_cli_eval_changed.tavl" "${out}")
execute_process(COMMAND "${CLI}" get ".items[1]" "${WORK_DIR}/tavl_cli_eval_changed.tavl"
  RESULT_VARIABLE read_rc OUTPUT_VARIABLE read_out ERROR_VARIABLE read_err)
if(NOT rc EQUAL 0 OR NOT read_rc EQUAL 0 OR NOT read_out STREQUAL "9\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval insert into adjacent values: rc=${rc}, stdout=${out}, read=${read_out}, stderr=${err}")
endif()
file(WRITE "${nested_arrays}" "matrix = [[1,3],[4,6]]\n")
execute_process(COMMAND "${CLI}" eval "insert(@data[0][*], 3, 2)" "${nested_arrays}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 3 OR NOT out STREQUAL "")
  message(FATAL_ERROR "eval insert out of range: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
file(WRITE "${nested_tuple}" "tuple = (1,3)\n")
execute_process(COMMAND "${CLI}" eval "insert(@data[0], 1, 9)" "${nested_tuple}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "tuple = (1,9, 3)\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval tuple insert: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
file(WRITE "${nested_object}" "obj = { a = 1, c = 3 }\n")
execute_process(COMMAND "${CLI}" eval "insert(@data[0], 1, b = 2)" "${nested_object}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "obj = { a = 1, b = 2, c = 3 }\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval object insert: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
file(WRITE "${nested_object}" "matrix = { a = { x = 1 }, b = { y = 2 } }\n")
execute_process(COMMAND "${CLI}" eval "upsert(@data[0][*].x, 9)" "${nested_object}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "matrix = { a = { x = 9 }, b = { y = 2, x = 9 } }\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval wildcard upsert: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" eval "upsert(@data[0].missing.x, 9)" "${nested_object}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 3 OR NOT out STREQUAL "")
  message(FATAL_ERROR "eval missing upsert parent: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" eval "upsert(@data[0][\"a.b\"], 9)" "${nested_object}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "\\\"a\\.b\\\" = 9" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval quoted upsert name: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
file(WRITE "${nested_object}" "obj = { x = 1, x = 2 }\n")
execute_process(COMMAND "${CLI}" eval "upsert(@data[0].x, 9)" "${nested_object}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT out STREQUAL "" OR NOT err MATCHES "err_duplicate_field")
  message(FATAL_ERROR "eval duplicate upsert name: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

set(standalone_edit "${WORK_DIR}/tavl_cli_standalone_edit.tavl")
file(WRITE "${standalone_edit}" "items = [1,3]\nobj = { a = 1 }\n")
execute_process(COMMAND "${CLI}" insert ".items" "1" "2" "${standalone_edit}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "items = [1,2, 3]\nobj = { a = 1 }\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "standalone insert: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" upsert ".obj.b" "2" "${standalone_edit}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "items = [1,3]\nobj = { a = 1, b = 2 }\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "standalone upsert add: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" upsert ".obj.a" "9" "${standalone_edit}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "items = [1,3]\nobj = { a = 9 }\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "standalone upsert replace: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" upsert ".new" "4" "${standalone_edit}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "items = [1,3]\nobj = { a = 1 }\nnew = 4\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "standalone upsert root: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" set ".window" "{ width =" "${valid}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT out STREQUAL "")
  message(FATAL_ERROR "invalid replacement must not print a document: rc=${rc}, stdout=${out}")
endif()

execute_process(COMMAND "${CLI}" get ".missing" "${valid}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 3)
  message(FATAL_ERROR "missing path: rc=${rc}")
endif()

execute_process(COMMAND "${CLI}" check "${unknown}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT err MATCHES "warning: unrecognized token")
  message(FATAL_ERROR "recoverable token: rc=${rc}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" check "${bad_string}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "error: err_unterminated_string")
  message(FATAL_ERROR "unterminated string: rc=${rc}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" check "${bad_escape}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "error: err_invalid_escape")
  message(FATAL_ERROR "invalid escape: rc=${rc}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" eval "where(true)" "${bad_escape}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT out STREQUAL "" OR NOT err MATCHES "error: err_invalid_escape")
  message(FATAL_ERROR "eval invalid escape: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

execute_process(COMMAND "${CLI}" check "${bad_comment}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "error: err_unterminated_block_comment")
  message(FATAL_ERROR "unterminated comment: rc=${rc}, stderr=${err}")
endif()

set(compare_source "${WORK_DIR}/tavl_cli_compare.tavl")
file(WRITE "${compare_source}" "value = 3\n")
foreach(case IN ITEMS ">|2" "<|4" "==|3" "!=|4" ">=|3" "<=|3")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 op)
  list(GET parts 1 rhs)
  execute_process(COMMAND "${CLI}" eval "where(@data[0] ${op} ${rhs})" "${compare_source}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0 OR NOT out STREQUAL "value = 3\n" OR NOT err STREQUAL "")
    message(FATAL_ERROR "eval comparison ${op}: rc=${rc}, stdout=${out}, stderr=${err}")
  endif()
endforeach()
foreach(case IN ITEMS ">|3" "<|3" "==|4" "!=|3" ">=|4" "<=|2")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 op)
  list(GET parts 1 rhs)
  execute_process(COMMAND "${CLI}" eval "where(@data[0] ${op} ${rhs})" "${compare_source}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0 OR NOT out STREQUAL "" OR NOT err STREQUAL "")
    message(FATAL_ERROR "eval false comparison ${op}: rc=${rc}, stdout=${out}, stderr=${err}")
  endif()
endforeach()

set(format_source "${WORK_DIR}/tavl_cli_format.tavl")
file(WRITE "${format_source}" "cfg={name=\"hello world\", values=10 20, matrix=[[1 2],[3 4]], child={a=1,b=2}}\n")
execute_process(COMMAND "${CLI}" format --output=compact "${format_source}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "cfg={name=\"hello world\",values=10 20,matrix=[[1 2],[3 4]],child={a=1,b=2}}\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "compact format: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" format --output=compact --names-depth=1 "${format_source}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "cfg=(\"hello world\",10 20,[[1 2],[3 4]],(1,2))\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "positional nested format: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" format --output=compact --names-depth=0 "${format_source}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "(\"hello world\",10 20,[[1 2],[3 4]],(1,2))\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "positional root format: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
file(WRITE "${WORK_DIR}/tavl_cli_formatted.tavl" "${out}")
execute_process(COMMAND "${CLI}" check "${WORK_DIR}/tavl_cli_formatted.tavl"
  RESULT_VARIABLE check_rc OUTPUT_VARIABLE check_out ERROR_VARIABLE check_err)
if(NOT check_rc EQUAL 0 OR NOT check_err STREQUAL "")
  message(FATAL_ERROR "positional format must parse: rc=${check_rc}, stderr=${check_err}")
endif()
execute_process(COMMAND "${CLI}" format --output=pretty "${format_source}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(expected_pretty "cfg = {\n  name = \"hello world\",\n  values = 10 20,\n  matrix = [\n    [\n      1 2\n    ],\n    [\n      3 4\n    ]\n  ],\n  child = {\n    a = 1,\n    b = 2\n  }\n}\n")
if(NOT rc EQUAL 0 OR NOT out STREQUAL expected_pretty OR NOT err STREQUAL "")
  message(FATAL_ERROR "pretty format: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" eval --output=compact "edit(@data[0]) { set(.name, ready) }" "${format_source}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "cfg={name=ready,values=10 20,matrix=[[1 2],[3 4]],child={a=1,b=2}}\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR "eval compact output: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
execute_process(COMMAND "${CLI}" format --output=preserve --names-depth=0 "${format_source}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 1 OR NOT out STREQUAL "" OR NOT err MATCHES "names-depth requires")
  message(FATAL_ERROR "incompatible output options: rc=${rc}, stdout=${out}, stderr=${err}")
endif()
file(WRITE "${WORK_DIR}/tavl_cli_format_trivia.tavl" "x = 1\n// trailing comment")
execute_process(COMMAND "${CLI}" format --output=preserve "${WORK_DIR}/tavl_cli_format_trivia.tavl"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "x = 1\n// trailing comment" OR NOT err STREQUAL "")
  message(FATAL_ERROR "preserve format: rc=${rc}, stdout=${out}, stderr=${err}")
endif()

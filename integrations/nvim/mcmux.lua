-- Source this file from init.lua to add mcmux commands inside its terminal panes.
local host = vim.env.MCMUX_HOST
local root = vim.env.MCMUX_PROJECT
local control = tonumber(vim.env.MCMUX_RUN_FD)
if not host or not root or not control then
  return
end

local function source_file()
  local file = vim.api.nvim_buf_get_name(0)
  if not file:match("%.c$") then
    error("Open a named C file first")
  end
  vim.cmd.update()
  return file
end

local function open_task(arguments)
  vim.cmd("botright new")
  local job = vim.fn.jobstart(arguments, { cwd = root, term = true })
  if job <= 0 then
    error("Could not start mcmux task")
  end
end

vim.api.nvim_create_user_command("McBuild", function()
  open_task({ "./nob", "build-c", source_file() })
end, { desc = "Save and build the current mcmux driver" })

vim.api.nvim_create_user_command("McRun", function()
  local file = source_file()
  vim.cmd("botright new")
  vim.bo.bufhidden = "hide"
  local channel = vim.api.nvim_open_term(0, {})
  local uv = vim.uv
  local output = uv.new_pipe(false)
  local errors = uv.new_pipe(false)
  local process
  local remaining = 2
  local status
  local emit = vim.schedule_wrap(function(data)
    pcall(vim.api.nvim_chan_send, channel, data)
  end)
  local function finish()
    if remaining == 0 and status then
      emit(("\n[mcmux driver exited %d]\n"):format(status))
    end
  end
  local function reader(pipe)
    return function(err, data)
      if data then
        emit(data)
      else
        pipe:close()
        remaining = remaining - 1
        if err then
          emit(err .. "\n")
        end
        finish()
      end
    end
  end
  local failure
  process, failure = uv.spawn(host, {
    args = { "--run-driver", file },
    cwd = root,
    env = { "MCMUX_RUN_FD=3" },
    stdio = { nil, output, errors, control },
  }, function(code, signal)
    status = signal == 0 and code or 128 + signal
    process:close()
    finish()
  end)
  if not process then
    output:close()
    errors:close()
    error("Could not request a driver run: " .. failure)
  end
  output:read_start(reader(output))
  errors:read_start(reader(errors))
end, { desc = "Save and run the current driver through the mcmux world session" })

vim.api.nvim_create_user_command("McModel", function(opts)
  if #opts.fargs > 0 then
    local args = { "./nob", "export-c" }
    vim.list_extend(args, opts.fargs)
    open_task(args)
    return
  end
  source_file()
  local arguments
  for _, line in ipairs(vim.api.nvim_buf_get_lines(0, 0, -1, false)) do
    local annotation = line:match("^%s*//%s*mcmux%-model:%s*(.-)%s*$")
    if annotation then
      if arguments then
        error("Keep one // mcmux-model: annotation")
      end
      local input, output, symbol = annotation:match("^(%S+)%s+(%S+)%s*(%S*)$")
      if not input or symbol == "" then
        error("Expected // mcmux-model: INPUT.mcs OUTPUT.c SYMBOL")
      end
      arguments = { "./nob", "export-c", input, output, symbol }
    end
  end
  if not arguments then
    error("Add // mcmux-model: INPUT.mcs OUTPUT.c SYMBOL or pass the arguments to :McModel")
  end
  open_task(arguments)
end, { nargs = "*", complete = "file", desc = "Export a generated C circuit model" })

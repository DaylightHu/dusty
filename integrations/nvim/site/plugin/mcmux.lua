-- Load the mcmux commands once per embedded Neovim session.
local root = vim.env.MCMUX_PROJECT
local control = tonumber(vim.env.MCMUX_RUN_FD)
if not root or not control or vim.g.mcmux_tasks_loaded or vim.fn.has("nvim-0.10") == 0 then
  return
end

local function source_file()
  local file = vim.api.nvim_buf_get_name(0)
  if vim.bo.buftype ~= "" or not file:match("%.c$") then
    error("Open a named C file first")
  end
  vim.cmd.update()
  return file
end

local function task_path(name)
  return root .. "/integrations/bin/" .. name
end

local function open_task(arguments)
  vim.cmd("botright new")
  local job
  if vim.fn.has("nvim-0.11") == 1 then
    job = vim.fn.jobstart(arguments, { cwd = root, term = true })
  else
    job = vim.fn.termopen(arguments, { cwd = root })
  end
  if job <= 0 then
    error("Could not start mcmux task")
  end
end

vim.api.nvim_create_user_command("McBuild", function()
  open_task({ task_path("mcmake"), source_file() })
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
  local environment = vim.fn.environ()
  environment.MCMUX_RUN_FD = "3"
  local env = {}
  for key, value in pairs(environment) do
    env[#env + 1] = key .. "=" .. value
  end
  local failure
  process, failure = uv.spawn(task_path("mcrun"), {
    args = { file },
    cwd = root,
    env = env,
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
  local args = { task_path("mcmodel") }
  if #opts.fargs > 0 then
    vim.list_extend(args, opts.fargs)
  else
    args[2] = source_file()
  end
  open_task(args)
end, { nargs = "*", complete = "file", desc = "Export a generated C circuit model" })

for shortcut, command in pairs({ mcmake = "McBuild", mcrun = "McRun", mcmodel = "McModel" }) do
  vim.cmd.cnoreabbrev({
    "<expr>",
    shortcut,
    ("getcmdtype() ==# ':' && getcmdline() ==# '%s' ? '%s' : '%s'"):format(shortcut, command, shortcut),
  })
end

vim.g.mcmux_tasks_loaded = true

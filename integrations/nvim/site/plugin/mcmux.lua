-- Optional commands for a user-managed Neovim session.
local root = vim.env.MCMUX_PROJECT
if vim.g.mcmux_tasks_loaded or vim.fn.has("nvim-0.10") == 0 then
  return
end

if not root or root == "" then
  local script = debug.getinfo(1, "S").source:sub(2)
  root = vim.fs.normalize(vim.fn.fnamemodify(script, ":p:h:h:h:h:h"))
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
  open_task({ task_path("mcrun"), source_file() })
end, { desc = "Save and run the current mcmux driver" })

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

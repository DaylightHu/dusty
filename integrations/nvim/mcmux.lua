-- Retain the entry point used by existing init.lua files.
if vim.env.MCMUX_PROJECT then
  dofile(vim.env.MCMUX_PROJECT .. "/integrations/nvim/site/plugin/mcmux.lua")
end

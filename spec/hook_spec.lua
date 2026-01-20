local hook = require "cluacov.hook"

describe("hook", function()
   describe("new", function()
      it("returns a function", function()
         local mock_runner = {
            initialized = false,
            data = {},
            configuration = { codefromstrings = false, savestepsize = 100 },
            file_included = function() return true end,
         }
         assert.is_function(hook.new(mock_runner))
      end)

      it("returns a callable debug hook", function()
         local mock_runner = {
            initialized = true,
            data = {},
            configuration = { codefromstrings = false, savestepsize = 100 },
            file_included = function() return true end,
         }
         local debug_hook = hook.new(mock_runner)
         -- Calling with invalid event should not error
         assert.has_no.errors(function()
            debug_hook("line", 1, 2)
         end)
      end)
   end)

   describe("tls_available", function()
      it("is a boolean", function()
         assert.is_boolean(hook.tls_available)
      end)

      it("indicates thread-local storage support", function()
         -- On modern systems with GCC/Clang/MSVC, TLS should be available
         -- This test documents the behavior, not enforces a specific value
         local tls = hook.tls_available
         assert.is_boolean(tls)
      end)
   end)

   describe("module structure", function()
      it("exports new function", function()
         assert.is_function(hook.new)
      end)

      it("exports tls_available field", function()
         assert.is_not_nil(hook.tls_available)
      end)
   end)
end)

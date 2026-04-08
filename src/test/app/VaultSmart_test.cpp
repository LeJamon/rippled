//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <test/jtx.h>
#include <test/jtx/vault.h>

#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Fees.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/jss.h>

namespace xrpl {
namespace test {

struct VaultSmart_test : public beast::unit_test::suite
{
    // Minimal WASM module with on_deposit and on_withdraw both returning 1.
    static std::string const vaultAllowWasmHex;

    // Minimal WASM module with on_deposit and on_withdraw both returning 0.
    static std::string const vaultRejectWasmHex;

    void
    testPreflight(FeatureBitset features)
    {
        testcase("VaultCreate preflight with VaultCode");

        using namespace jtx;

        Account const alice{"alice"};

        {
            // featureSmartVault disabled: VaultCode + policy 2 not allowed
            Env env(*this, features - featureSmartVault);
            env.fund(XRP(5000), alice);
            Vault vault{env};
            auto [tx, keylet] =
                vault.create({.owner = alice, .asset = xrpIssue()});
            tx[sfVaultCode] = vaultAllowWasmHex;
            tx[sfWithdrawalPolicy] = vaultStrategyWASM;
            env(tx, ter(temDISABLED));
        }

        {
            // Empty VaultCode
            Env env(*this, features);
            env.fund(XRP(5000), alice);
            Vault vault{env};
            auto [tx, keylet] =
                vault.create({.owner = alice, .asset = xrpIssue()});
            tx[sfVaultCode] = "";
            tx[sfWithdrawalPolicy] = vaultStrategyWASM;
            env(tx, ter(temMALFORMED));
        }

        {
            // VaultCode too large
            Env env(
                *this,
                envconfig([](std::unique_ptr<Config> cfg) {
                    cfg->FEES.extension_size_limit = 10;  // 10 bytes max
                    return cfg;
                }),
                features);
            env.fund(XRP(5000), alice);
            Vault vault{env};
            auto [tx, keylet] =
                vault.create({.owner = alice, .asset = xrpIssue()});
            // 11-byte hex string
            tx[sfVaultCode] = "00112233445566778899AA";
            tx[sfWithdrawalPolicy] = vaultStrategyWASM;
            env(tx, ter(temMALFORMED));
        }

        {
            // Invalid WASM (missing on_deposit export)
            Env env(*this, features);
            env.fund(XRP(5000), alice);
            Vault vault{env};
            auto [tx, keylet] =
                vault.create({.owner = alice, .asset = xrpIssue()});
            // Minimal valid WASM module with no exports
            tx[sfVaultCode] = "0061736d01000000";
            tx[sfWithdrawalPolicy] = vaultStrategyWASM;
            env(tx, ter(temBAD_WASM));
        }

        {
            // Valid VaultCode with policy 2 — should succeed
            Env env(*this, features);
            env.fund(XRP(10000), alice);
            Vault vault{env};
            auto [tx, keylet] =
                vault.create({.owner = alice, .asset = xrpIssue()});
            tx[sfVaultCode] = vaultAllowWasmHex;
            tx[sfWithdrawalPolicy] = vaultStrategyWASM;
            XRPAmount const createFee = env.current()->fees().increment +
                9 * env.current()->fees().base +
                5 * static_cast<int>(vaultAllowWasmHex.size() / 2);
            env(tx, fee(createFee));
            BEAST_EXPECT(env.le(keylet));

            auto const sle = env.le(keylet);
            if (BEAST_EXPECT(sle))
                BEAST_EXPECT(sle->isFieldPresent(sfVaultCode));
        }
    }

    void
    testDepositChecks(FeatureBitset features)
    {
        testcase("VaultDeposit smart vault checks");

        using namespace jtx;

        Account const alice{"alice"};

        // Advance past the flag ledger so FeeSettings (including gasPrice)
        // are updated.
        Env env(*this, features);
        for (auto i = env.current()->seq(); i <= 257; ++i)
            env.close();

        env.fund(XRP(10000), alice);

        Vault vault{env};

        // Create a smart vault
        auto [createTx, vaultKeylet] =
            vault.create({.owner = alice, .asset = xrpIssue()});
        createTx[sfVaultCode] = vaultAllowWasmHex;
        createTx[sfWithdrawalPolicy] = vaultStrategyWASM;
        {
            XRPAmount const createFee = env.current()->fees().increment +
                9 * env.current()->fees().base +
                5 * static_cast<int>(vaultAllowWasmHex.size() / 2);
            env(createTx, fee(createFee));
        }
        env.close();

        auto const vaultId = vaultKeylet.key;

        // Create a plain vault (no VaultCode)
        auto [createTx2, vaultKeylet2] =
            vault.create({.owner = alice, .asset = xrpIssue()});
        env(createTx2);
        env.close();
        auto const plainVaultId = vaultKeylet2.key;

        {
            // Missing ComputationAllowance on smart vault
            auto dtx = vault.deposit(
                {.depositor = alice, .id = vaultId, .amount = XRP(100)});
            env(dtx, ter(tefWASM_FIELD_NOT_INCLUDED));
        }

        {
            // ComputationAllowance present on plain vault
            std::uint32_t const allowance = 100;
            XRPAmount const depositFee = env.current()->fees().base +
                (allowance * env.current()->fees().gasPrice) /
                    MICRO_DROPS_PER_DROP +
                1;
            auto dtx = vault.deposit(
                {.depositor = alice, .id = plainVaultId, .amount = XRP(100)});
            dtx[sfComputationAllowance] = allowance;
            env(dtx, fee(depositFee), ter(tefNO_WASM));
        }

        {
            // featureSmartVault disabled: ComputationAllowance not allowed
            Env env2(*this, features - featureSmartVault);
            for (auto i = env2.current()->seq(); i <= 257; ++i)
                env2.close();
            env2.fund(XRP(10000), alice);
            Vault vault2{env2};
            auto [ctxPlain, kl] =
                vault2.create({.owner = alice, .asset = xrpIssue()});
            env2(ctxPlain);
            env2.close();

            std::uint32_t const allowance = 100;
            XRPAmount const depositFee = env2.current()->fees().base +
                (allowance * env2.current()->fees().gasPrice) /
                    MICRO_DROPS_PER_DROP +
                1;
            auto dtx = vault2.deposit(
                {.depositor = alice, .id = kl.key, .amount = XRP(100)});
            dtx[sfComputationAllowance] = allowance;
            env2(dtx, fee(depositFee), ter(temDISABLED));
        }

        {
            // WASM rejects the deposit (on_deposit returns 0)
            auto [rejectTx, rejectKeylet] =
                vault.create({.owner = alice, .asset = xrpIssue()});
            rejectTx[sfVaultCode] = vaultRejectWasmHex;
            rejectTx[sfWithdrawalPolicy] = vaultStrategyWASM;
            {
                XRPAmount const createFee = env.current()->fees().increment +
                    9 * env.current()->fees().base +
                    5 * static_cast<int>(vaultRejectWasmHex.size() / 2);
                env(rejectTx, fee(createFee));
            }
            env.close();

            std::uint32_t const allowance = 100;
            XRPAmount const depositFee = env.current()->fees().base +
                (allowance * env.current()->fees().gasPrice) /
                    MICRO_DROPS_PER_DROP +
                1;
            auto dtx = vault.deposit(
                {.depositor = alice,
                 .id = rejectKeylet.key,
                 .amount = XRP(100)});
            dtx[sfComputationAllowance] = allowance;
            env(dtx, fee(depositFee), ter(tecWASM_REJECTED));

            auto const txMeta = env.meta();
            BEAST_EXPECT(txMeta->isFieldPresent(sfGasUsed));
            if (BEAST_EXPECT(txMeta->isFieldPresent(sfWasmReturnCode)))
                BEAST_EXPECT(txMeta->getFieldI32(sfWasmReturnCode) == 0);
        }

        {
            // Successful deposit with WASM
            std::uint32_t const allowance = 100;
            XRPAmount const depositFee = env.current()->fees().base +
                (allowance * env.current()->fees().gasPrice) /
                    MICRO_DROPS_PER_DROP +
                1;
            auto dtx = vault.deposit(
                {.depositor = alice, .id = vaultId, .amount = XRP(100)});
            dtx[sfComputationAllowance] = allowance;
            env(dtx, fee(depositFee));

            auto const txMeta = env.meta();
            BEAST_EXPECT(txMeta->isFieldPresent(sfGasUsed));
            if (BEAST_EXPECT(txMeta->isFieldPresent(sfWasmReturnCode)))
                BEAST_EXPECT(txMeta->getFieldI32(sfWasmReturnCode) == 1);
        }
    }

    void
    testWithdrawChecks(FeatureBitset features)
    {
        testcase("VaultWithdraw smart vault checks");

        using namespace jtx;

        Account const alice{"alice"};

        Env env(*this, features);
        for (auto i = env.current()->seq(); i <= 257; ++i)
            env.close();

        env.fund(XRP(10000), alice);
        Vault vault{env};

        // Create a smart vault and deposit into it first
        auto [createTx, vaultKeylet] =
            vault.create({.owner = alice, .asset = xrpIssue()});
        createTx[sfVaultCode] = vaultAllowWasmHex;
        createTx[sfWithdrawalPolicy] = vaultStrategyWASM;
        {
            XRPAmount const createFee = env.current()->fees().increment +
                9 * env.current()->fees().base +
                5 * static_cast<int>(vaultAllowWasmHex.size() / 2);
            env(createTx, fee(createFee));
        }
        env.close();

        auto const vaultId = vaultKeylet.key;
        std::uint32_t const allowance = 100;
        XRPAmount const opFee = env.current()->fees().base +
            (allowance * env.current()->fees().gasPrice) /
                MICRO_DROPS_PER_DROP +
            1;

        // Deposit first so there are assets to withdraw
        auto dtx =
            vault.deposit({.depositor = alice, .id = vaultId, .amount = XRP(500)});
        dtx[sfComputationAllowance] = allowance;
        env(dtx, fee(opFee));
        env.close();

        {
            // Missing ComputationAllowance on withdraw from smart vault
            auto wtx = vault.withdraw(
                {.depositor = alice, .id = vaultId, .amount = XRP(10)});
            env(wtx, ter(tefWASM_FIELD_NOT_INCLUDED));
        }

        {
            // ComputationAllowance present on plain vault withdraw
            auto [ctxPlain, klPlain] =
                vault.create({.owner = alice, .asset = xrpIssue()});
            env(ctxPlain);
            env.close();

            auto wtx = vault.withdraw(
                {.depositor = alice, .id = klPlain.key, .amount = XRP(1)});
            wtx[sfComputationAllowance] = allowance;
            env(wtx, fee(opFee), ter(tefNO_WASM));
        }

        {
            // Successful withdraw with WASM
            auto wtx = vault.withdraw(
                {.depositor = alice, .id = vaultId, .amount = XRP(10)});
            wtx[sfComputationAllowance] = allowance;
            env(wtx, fee(opFee));

            auto const txMeta = env.meta();
            BEAST_EXPECT(txMeta->isFieldPresent(sfGasUsed));
            if (BEAST_EXPECT(txMeta->isFieldPresent(sfWasmReturnCode)))
                BEAST_EXPECT(txMeta->getFieldI32(sfWasmReturnCode) == 1);
        }
    }

    void
    testReserveIncrements(FeatureBitset features)
    {
        testcase("VaultCreate reserve increments with VaultCode");

        using namespace jtx;

        Account const alice{"alice"};

        Env env(*this, features);
        env.fund(XRP(100000), alice);
        Vault vault{env};

        auto const ownerCountBefore = env.ownerCount(alice);

        // Plain vault: 1 increment
        {
            auto [tx, keylet] =
                vault.create({.owner = alice, .asset = xrpIssue()});
            env(tx);
            env.close();
            // +1 for vault object, +1 for MPToken = 2 total
            BEAST_EXPECT(env.ownerCount(alice) == ownerCountBefore + 2);
        }

        // Smart vault with code < 500 bytes: still 1 increment
        {
            BEAST_EXPECT(vaultAllowWasmHex.size() / 2 < 500);
            auto [tx, keylet] =
                vault.create({.owner = alice, .asset = xrpIssue()});
            tx[sfVaultCode] = vaultAllowWasmHex;
            tx[sfWithdrawalPolicy] = vaultStrategyWASM;
            XRPAmount const createFee = env.current()->fees().increment +
                9 * env.current()->fees().base +
                5 * static_cast<int>(vaultAllowWasmHex.size() / 2);
            env(tx, fee(createFee));
            env.close();
            // +2 per vault (1 vault object + 1 MPToken), two vaults created
            BEAST_EXPECT(env.ownerCount(alice) == ownerCountBefore + 4);
        }
    }

    void
    testSmartVaultDelete(FeatureBitset features)
    {
        testcase("VaultDelete restores reserve for smart vault");

        using namespace jtx;

        Account const alice{"alice"};

        Env env(*this, features);
        for (auto i = env.current()->seq(); i <= 257; ++i)
            env.close();

        env.fund(XRP(10000), alice);
        Vault vault{env};

        auto const ownerCountBefore = env.ownerCount(alice);

        // Create smart vault
        auto [createTx, vaultKeylet] =
            vault.create({.owner = alice, .asset = xrpIssue()});
        createTx[sfVaultCode] = vaultAllowWasmHex;
        createTx[sfWithdrawalPolicy] = vaultStrategyWASM;
        {
            XRPAmount const createFee = env.current()->fees().increment +
                9 * env.current()->fees().base +
                5 * static_cast<int>(vaultAllowWasmHex.size() / 2);
            env(createTx, fee(createFee));
        }
        env.close();

        auto const ownerCountAfterCreate = env.ownerCount(alice);
        BEAST_EXPECT(ownerCountAfterCreate == ownerCountBefore + 2);

        // Deposit then withdraw all to empty the vault
        std::uint32_t const allowance = 100;
        XRPAmount const opFee = env.current()->fees().base +
            (allowance * env.current()->fees().gasPrice) /
                MICRO_DROPS_PER_DROP +
            1;

        auto dtx = vault.deposit(
            {.depositor = alice,
             .id = vaultKeylet.key,
             .amount = XRP(100)});
        dtx[sfComputationAllowance] = allowance;
        env(dtx, fee(opFee));
        env.close();

        auto wtx = vault.withdraw(
            {.depositor = alice,
             .id = vaultKeylet.key,
             .amount = XRP(100)});
        wtx[sfComputationAllowance] = allowance;
        env(wtx, fee(opFee));
        env.close();

        // Delete the smart vault
        auto deleteTx =
            vault.del({.owner = alice, .id = vaultKeylet.key});
        env(deleteTx);
        env.close();

        // Owner count should be restored to before-create value
        BEAST_EXPECT(env.ownerCount(alice) == ownerCountBefore);

        // Vault should no longer exist
        BEAST_EXPECT(!env.le(vaultKeylet));
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testPreflight(features);
        testDepositChecks(features);
        testWithdrawChecks(features);
        testReserveIncrements(features);
        testSmartVaultDelete(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{testable_amendments()};
        testWithFeats(all);
    }
};

// Minimal WASM: two functions (on_deposit, on_withdraw) both returning i32 1
std::string const VaultSmart_test::vaultAllowWasmHex =
    "0061736d01000000"
    "0105016000017f"
    "0303020000"
    "071c02"
    "0a6f6e5f6465706f73697400"
    "000b6f6e5f7769746864726177"
    "0001"
    "0a0b02"
    "040041010b"
    "040041010b";

// Same but both functions return i32 0 (reject)
std::string const VaultSmart_test::vaultRejectWasmHex =
    "0061736d01000000"
    "0105016000017f"
    "0303020000"
    "071c02"
    "0a6f6e5f6465706f73697400"
    "000b6f6e5f7769746864726177"
    "0001"
    "0a0b02"
    "040041000b"
    "040041000b";

BEAST_DEFINE_TESTSUITE(VaultSmart, app, xrpl);

}  // namespace test
}  // namespace xrpl

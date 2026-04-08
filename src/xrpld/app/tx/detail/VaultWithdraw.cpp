#include <xrpld/app/tx/detail/VaultWithdraw.h>
#include <xrpld/app/wasm/HostFuncImpl.h>
#include <xrpld/app/wasm/WasmVM.h>

#include <xrpl/ledger/CredentialHelpers.h>
#include <xrpl/ledger/View.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Fees.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/TxFlags.h>

namespace xrpl {

bool
VaultWithdraw::checkExtraFeatures(PreflightContext const& ctx)
{
    if (ctx.tx.isFieldPresent(sfComputationAllowance) &&
        !ctx.rules.enabled(featureSmartVault))
        return false;
    return true;
}

XRPAmount
VaultWithdraw::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    auto baseFee = Transactor::calculateBaseFee(view, tx);

    if (auto const allowance = tx[~sfComputationAllowance])
    {
        auto const& fees = view.fees();
        auto const gasPrice = fees.gasPrice;
        baseFee += XRPAmount{
            static_cast<XRPAmount::value_type>(
                (*allowance * gasPrice) / MICRO_DROPS_PER_DROP + 1)};
    }

    return baseFee;
}

NotTEC
VaultWithdraw::preflight(PreflightContext const& ctx)
{
    if (ctx.tx[sfVaultID] == beast::zero)
    {
        JLOG(ctx.j.debug()) << "VaultWithdraw: zero/empty vault ID.";
        return temMALFORMED;
    }

    if (ctx.tx[sfAmount] <= beast::zero)
        return temBAD_AMOUNT;

    if (auto const destination = ctx.tx[~sfDestination])
    {
        if (*destination == beast::zero)
        {
            return temMALFORMED;
        }
    }

    return tesSUCCESS;
}

TER
VaultWithdraw::preclaim(PreclaimContext const& ctx)
{
    auto const vault = ctx.view.read(keylet::vault(ctx.tx[sfVaultID]));
    if (!vault)
        return tecNO_ENTRY;

    // WASM policy requires ComputationAllowance; non-WASM rejects it
    bool const isWASM =
        vault->at(sfWithdrawalPolicy) == vaultStrategyWASM;
    if (isWASM && !ctx.tx.isFieldPresent(sfComputationAllowance))
        return tefWASM_FIELD_NOT_INCLUDED;
    if (!isWASM && ctx.tx.isFieldPresent(sfComputationAllowance))
        return tefNO_WASM;

    auto const assets = ctx.tx[sfAmount];
    auto const vaultAsset = vault->at(sfAsset);
    auto const vaultShare = vault->at(sfShareMPTID);
    if (assets.asset() != vaultAsset && assets.asset() != vaultShare)
        return tecWRONG_ASSET;

    auto const& vaultAccount = vault->at(sfAccount);
    auto const& account = ctx.tx[sfAccount];
    auto const& dstAcct = ctx.tx[~sfDestination].value_or(account);
    if (auto ter = canTransfer(ctx.view, vaultAsset, vaultAccount, dstAcct);
        !isTesSuccess(ter))
    {
        JLOG(ctx.j.debug())
            << "VaultWithdraw: vault assets are non-transferable.";
        return ter;
    }

    // Enforce valid withdrawal policy
    if (vault->at(sfWithdrawalPolicy) != vaultStrategyFirstComeFirstServe &&
        vault->at(sfWithdrawalPolicy) != vaultStrategyWASM)
    {
        // LCOV_EXCL_START
        JLOG(ctx.j.error()) << "VaultWithdraw: invalid withdrawal policy.";
        return tefINTERNAL;
        // LCOV_EXCL_STOP
    }

    if (auto const ret = canWithdraw(ctx.view, ctx.tx))
        return ret;

    // If sending to Account (i.e. not a transfer), we will also create (only
    // if authorized) a trust line or MPToken as needed, in doApply().
    // Destination MPToken or trust line must exist if _not_ sending to Account.
    AuthType const authType =
        account == dstAcct ? AuthType::WeakAuth : AuthType::StrongAuth;
    if (auto const ter = requireAuth(ctx.view, vaultAsset, dstAcct, authType);
        !isTesSuccess(ter))
        return ter;

    // Cannot withdraw from a Vault an Asset frozen for the destination account
    if (auto const ret = checkFrozen(ctx.view, dstAcct, vaultAsset))
        return ret;

    // Cannot return shares to the vault, if the underlying asset was frozen for
    // the submitter
    if (auto const ret = checkFrozen(ctx.view, account, vaultShare))
        return ret;

    return tesSUCCESS;
}

TER
VaultWithdraw::doApply()
{
    auto const vault = view().peek(keylet::vault(ctx_.tx[sfVaultID]));
    if (!vault)
        return tefINTERNAL;  // LCOV_EXCL_LINE

    // WASM withdrawal path: run on_withdraw, transfer assets directly
    if (vault->at(sfWithdrawalPolicy) == vaultStrategyWASM)
    {
        auto const amount = ctx_.tx[sfAmount];
        auto const& wasmCode = vault->getFieldVL(sfVaultCode);
        auto const vaultKey = keylet::vault(ctx_.tx[sfVaultID]);
        WasmHostFunctionsImpl hfs(ctx_, vaultKey);
        auto const gasLimit = static_cast<int64_t>(
            ctx_.tx[sfComputationAllowance]);

        auto const result = runEscrowWasm(
            wasmCode, hfs, VAULT_WITHDRAW_FUNCTION, {}, gasLimit);
        if (!result)
            return result.error();

        auto const& [retVal, gasUsed] = *result;

        // Persist updated data from WASM
        if (auto const& data = hfs.getData())
        {
            auto vaultMut = view().peek(vaultKey);
            vaultMut->setFieldVL(sfData, *data);
            view().update(vaultMut);
        }

        // Set metadata
        ctx_.setWasmReturnCode(retVal);
        ctx_.setGasUsed(gasUsed);

        if (retVal <= 0)
            return tecWASM_REJECTED;

        // Direct asset transfer for WASM vaults (no shares)
        auto const& vaultAccount = vault->at(sfAccount);
        auto const destination = ctx_.tx[~sfDestination].value_or(account_);

        vault->at(sfAssetsTotal) -= amount;
        vault->at(sfAssetsAvailable) -= amount;
        view().update(vault);

        if (auto const ter = accountSend(
                view(), vaultAccount, destination, amount, j_,
                WaiveTransferFee::Yes);
            !isTesSuccess(ter))
            return ter;

        return tesSUCCESS;
    }

    auto const mptIssuanceID = *((*vault)[sfShareMPTID]);
    auto const sleIssuance = view().read(keylet::mptIssuance(mptIssuanceID));
    if (!sleIssuance)
    {
        // LCOV_EXCL_START
        JLOG(j_.error()) << "VaultWithdraw: missing issuance of vault shares.";
        return tefINTERNAL;
        // LCOV_EXCL_STOP
    }

    // Note, we intentionally do not check lsfVaultPrivate flag on the Vault. If
    // you have a share in the vault, it means you were at some point authorized
    // to deposit into it, and this means you are also indefinitely authorized
    // to withdraw from it.

    auto const amount = ctx_.tx[sfAmount];
    Asset const vaultAsset = vault->at(sfAsset);
    MPTIssue const share{mptIssuanceID};
    STAmount sharesRedeemed = {share};
    STAmount assetsWithdrawn;
    try
    {
        if (amount.asset() == vaultAsset)
        {
            // Fixed assets, variable shares.
            {
                auto const maybeShares =
                    assetsToSharesWithdraw(vault, sleIssuance, amount);
                if (!maybeShares)
                    return tecINTERNAL;  // LCOV_EXCL_LINE
                sharesRedeemed = *maybeShares;
            }

            if (sharesRedeemed == beast::zero)
                return tecPRECISION_LOSS;
            auto const maybeAssets =
                sharesToAssetsWithdraw(vault, sleIssuance, sharesRedeemed);
            if (!maybeAssets)
                return tecINTERNAL;  // LCOV_EXCL_LINE
            assetsWithdrawn = *maybeAssets;
        }
        else if (amount.asset() == share)
        {
            // Fixed shares, variable assets.
            sharesRedeemed = amount;
            auto const maybeAssets =
                sharesToAssetsWithdraw(vault, sleIssuance, sharesRedeemed);
            if (!maybeAssets)
                return tecINTERNAL;  // LCOV_EXCL_LINE
            assetsWithdrawn = *maybeAssets;
        }
        else
            return tefINTERNAL;  // LCOV_EXCL_LINE
    }
    catch (std::overflow_error const&)
    {
        // It's easy to hit this exception from Number with large enough Scale
        // so we avoid spamming the log and only use debug here.
        JLOG(j_.debug())  //
            << "VaultWithdraw: overflow error with"
            << " scale=" << (int)vault->at(sfScale).value()  //
            << ", assetsTotal=" << vault->at(sfAssetsTotal).value()
            << ", sharesTotal=" << sleIssuance->at(sfOutstandingAmount)
            << ", amount=" << amount.value();
        return tecPATH_DRY;
    }

    if (accountHolds(
            view(),
            account_,
            share,
            FreezeHandling::fhZERO_IF_FROZEN,
            AuthHandling::ahIGNORE_AUTH,
            j_) < sharesRedeemed)
    {
        JLOG(j_.debug()) << "VaultWithdraw: account doesn't hold enough shares";
        return tecINSUFFICIENT_FUNDS;
    }

    auto assetsAvailable = vault->at(sfAssetsAvailable);
    auto assetsTotal = vault->at(sfAssetsTotal);
    [[maybe_unused]] auto const lossUnrealized = vault->at(sfLossUnrealized);
    XRPL_ASSERT(
        lossUnrealized <= (assetsTotal - assetsAvailable),
        "xrpl::VaultWithdraw::doApply : loss and assets do balance");

    // The vault must have enough assets on hand. The vault may hold assets
    // that it has already pledged. That is why we look at AssetAvailable
    // instead of the pseudo-account balance.
    if (*assetsAvailable < assetsWithdrawn)
    {
        JLOG(j_.debug()) << "VaultWithdraw: vault doesn't hold enough assets";
        return tecINSUFFICIENT_FUNDS;
    }

    assetsTotal -= assetsWithdrawn;
    assetsAvailable -= assetsWithdrawn;
    view().update(vault);

    auto const& vaultAccount = vault->at(sfAccount);
    // Transfer shares from depositor to vault.
    if (auto const ter = accountSend(
            view(),
            account_,
            vaultAccount,
            sharesRedeemed,
            j_,
            WaiveTransferFee::Yes);
        !isTesSuccess(ter))
        return ter;

    // Try to remove MPToken for shares, if the account balance is zero. Vault
    // pseudo-account will never set lsfMPTAuthorized, so we ignore flags.
    // Keep MPToken if holder is the vault owner.
    if (account_ != vault->at(sfOwner))
    {
        if (auto const ter = removeEmptyHolding(
                view(), account_, sharesRedeemed.asset(), j_);
            isTesSuccess(ter))
        {
            JLOG(j_.debug())  //
                << "VaultWithdraw: removed empty MPToken for vault shares"
                << " MPTID=" << to_string(mptIssuanceID)  //
                << " account=" << toBase58(account_);
        }
        else if (ter != tecHAS_OBLIGATIONS)
        {
            // LCOV_EXCL_START
            JLOG(j_.error())  //
                << "VaultWithdraw: failed to remove MPToken for vault shares"
                << " MPTID=" << to_string(mptIssuanceID)  //
                << " account=" << toBase58(account_)      //
                << " with result: " << transToken(ter);
            return ter;
            // LCOV_EXCL_STOP
        }
        // else quietly ignore, account balance is not zero
    }

    auto const dstAcct = ctx_.tx[~sfDestination].value_or(account_);

    return doWithdraw(
        view(),
        ctx_.tx,
        account_,
        dstAcct,
        vaultAccount,
        mPriorBalance,
        assetsWithdrawn,
        j_);
}

}  // namespace xrpl

#include <xrpld/app/tx/detail/MPTokenAuthorize.h>
#include <xrpld/app/tx/detail/MPTokenIssuanceCreate.h>
#include <xrpld/app/tx/detail/VaultCreate.h>
#include <xrpld/app/wasm/HostFuncImpl.h>
#include <xrpld/app/wasm/WasmVM.h>

#include <xrpl/ledger/View.h>
#include <xrpl/protocol/Asset.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/MPTIssue.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/TxFlags.h>

namespace xrpl {

std::int32_t
vaultReserveIncrements(std::optional<Slice> const& vaultCode)
{
    if (!vaultCode)
        return 1;
    return 1 + static_cast<std::int32_t>(vaultCode->size()) / 500;
}

bool
VaultCreate::checkExtraFeatures(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureMPTokensV1))
        return false;

    if (ctx.tx.isFieldPresent(sfDomainID) &&
        !ctx.rules.enabled(featurePermissionedDomains))
        return false;

    if ((ctx.tx.isFieldPresent(sfVaultCode) ||
         ctx.tx[~sfWithdrawalPolicy] == vaultStrategyWASM) &&
        !ctx.rules.enabled(featureSmartVault))
        return false;

    return true;
}

std::uint32_t
VaultCreate::getFlagsMask(PreflightContext const& ctx)
{
    return tfVaultCreateMask;
}

NotTEC
VaultCreate::preflight(PreflightContext const& ctx)
{
    if (!validDataLength(ctx.tx[~sfData], maxDataPayloadLength))
        return temMALFORMED;

    if (auto const withdrawalPolicy = ctx.tx[~sfWithdrawalPolicy])
    {
        // Enforce valid withdrawal policy
        if (*withdrawalPolicy != vaultStrategyFirstComeFirstServe &&
            *withdrawalPolicy != vaultStrategyWASM)
            return temMALFORMED;

        // WASM policy requires VaultCode and vice versa
        bool const hasVaultCode = ctx.tx.isFieldPresent(sfVaultCode);
        if (*withdrawalPolicy == vaultStrategyWASM && !hasVaultCode)
            return temMALFORMED;
        if (hasVaultCode && *withdrawalPolicy != vaultStrategyWASM)
            return temMALFORMED;
    }

    // Validate WASM bytecode if present
    if (ctx.tx.isFieldPresent(sfVaultCode))
    {
        auto const& code = ctx.tx.getFieldVL(sfVaultCode);
        if (code.empty())
            return temMALFORMED;

        auto const maxSize =
            ctx.app.config().FEES.extension_size_limit;
        if (maxSize && code.size() > *maxSize)
            return temMALFORMED;

        // Validate WASM exports "on_deposit" and "on_withdraw"
        WasmHostFunctionsImpl hfs(beast::Journal{beast::Journal::getNullSink()});
        if (auto const tec = preflightEscrowWasm(
                code, hfs, VAULT_DEPOSIT_FUNCTION);
            tec != tesSUCCESS)
            return tec;
        if (auto const tec = preflightEscrowWasm(
                code, hfs, VAULT_WITHDRAW_FUNCTION);
            tec != tesSUCCESS)
            return tec;
    }

    if (auto const domain = ctx.tx[~sfDomainID])
    {
        if (*domain == beast::zero)
            return temMALFORMED;
        else if ((ctx.tx.getFlags() & tfVaultPrivate) == 0)
            return temMALFORMED;  // DomainID only allowed on private vaults
    }

    if (auto const assetMax = ctx.tx[~sfAssetsMaximum])
    {
        if (*assetMax < beast::zero)
            return temMALFORMED;
    }

    if (auto const metadata = ctx.tx[~sfMPTokenMetadata])
    {
        if (metadata->length() == 0 ||
            metadata->length() > maxMPTokenMetadataLength)
            return temMALFORMED;
    }

    if (auto const scale = ctx.tx[~sfScale])
    {
        auto const vaultAsset = ctx.tx[sfAsset];
        if (vaultAsset.holds<MPTIssue>() || vaultAsset.native())
            return temMALFORMED;

        if (scale > vaultMaximumIOUScale)
            return temMALFORMED;
    }

    return tesSUCCESS;
}

XRPAmount
VaultCreate::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    auto baseFee = calculateOwnerReserveFee(view, tx);

    if (tx.isFieldPresent(sfVaultCode))
    {
        auto const& code = tx.getFieldVL(sfVaultCode);
        baseFee += XRPAmount{
            static_cast<XRPAmount::value_type>(
                9 * view.fees().base + 5 * code.size())};
    }

    return baseFee;
}

TER
VaultCreate::preclaim(PreclaimContext const& ctx)
{
    auto const vaultAsset = ctx.tx[sfAsset];
    auto const account = ctx.tx[sfAccount];

    if (auto const ter = canAddHolding(ctx.view, vaultAsset))
        return ter;

    // Check for pseudo-account issuers - we do not want a vault to hold such
    // assets (e.g. MPT shares to other vaults or AMM LPTokens) as they would be
    // impossible to clawback (should the need arise)
    if (!vaultAsset.native())
    {
        if (isPseudoAccount(ctx.view, vaultAsset.getIssuer()))
            return tecWRONG_ASSET;
    }

    // Cannot create Vault for an Asset frozen for the vault owner
    if (isFrozen(ctx.view, account, vaultAsset))
        return vaultAsset.holds<Issue>() ? tecFROZEN : tecLOCKED;

    if (auto const domain = ctx.tx[~sfDomainID])
    {
        auto const sleDomain =
            ctx.view.read(keylet::permissionedDomain(*domain));
        if (!sleDomain)
            return tecOBJECT_NOT_FOUND;
    }

    auto const sequence = ctx.tx.getSeqValue();
    if (auto const accountId = pseudoAccountAddress(
            ctx.view, keylet::vault(account, sequence).key);
        accountId == beast::zero)
        return terADDRESS_COLLISION;

    return tesSUCCESS;
}

TER
VaultCreate::doApply()
{
    // All return codes in `doApply` must be `tec`, `ter`, or `tes`.
    // As we move checks into `preflight` and `preclaim`,
    // we can consider downgrading them to `tef` or `tem`.

    auto const& tx = ctx_.tx;
    auto const sequence = tx.getSeqValue();
    auto const owner = view().peek(keylet::account(account_));
    if (owner == nullptr)
        return tefINTERNAL;  // LCOV_EXCL_LINE

    auto vault = std::make_shared<SLE>(keylet::vault(account_, sequence));

    if (auto ter = dirLink(view(), account_, vault))
        return ter;
    // Variable reserve: plain vault = 1 increment + 1 for pseudo account,
    // WASM vault adds increments based on code size
    auto const reserveIncrements =
        1 + vaultReserveIncrements(tx[~sfVaultCode]);
    adjustOwnerCount(view(), owner, reserveIncrements, j_);
    auto const ownerCount = owner->at(sfOwnerCount);
    if (mPriorBalance < view().fees().accountReserve(ownerCount))
        return tecINSUFFICIENT_RESERVE;

    auto maybePseudo = createPseudoAccount(view(), vault->key(), sfVaultID);
    if (!maybePseudo)
        return maybePseudo.error();  // LCOV_EXCL_LINE
    auto& pseudo = *maybePseudo;
    auto pseudoId = pseudo->at(sfAccount);
    auto asset = tx[sfAsset];

    Issue const issue = asset.get<Issue>();
    STAmount limit{issue, 0};
    if (auto ter = addEmptyHolding(view(), pseudoId, mPriorBalance, asset, j_);
        !isTesSuccess(ter))
        return ter;

    std::uint8_t const scale = (asset.holds<MPTIssue>() || asset.native())
        ? 0
        : ctx_.tx[~sfScale].value_or(vaultDefaultIOUScale);

    auto txFlags = tx.getFlags();
    std::uint32_t mptFlags = 0;
    if ((txFlags & tfVaultShareNonTransferable) == 0)
        mptFlags |= (lsfMPTCanEscrow | lsfMPTCanTrade | lsfMPTCanTransfer);
    if (txFlags & tfVaultPrivate)
        mptFlags |= lsfMPTRequireAuth;

    // Note, here we are **not** creating an MPToken for the assets held in
    // the vault. That MPToken or TrustLine/RippleState is created above, in
    // addEmptyHolding. Here we are creating MPTokenIssuance for the shares
    // in the vault
    auto maybeShare = MPTokenIssuanceCreate::create(
        view(),
        j_,
        {
            .priorBalance = std::nullopt,
            .account = pseudoId->value(),
            .sequence = 1,
            .flags = mptFlags,
            .assetScale = scale,
            .metadata = tx[~sfMPTokenMetadata],
            .domainId = tx[~sfDomainID],
        });
    if (!maybeShare)
        return maybeShare.error();  // LCOV_EXCL_LINE
    auto const& mptIssuanceID = *maybeShare;

    vault->setFieldIssue(sfAsset, STIssue{sfAsset, asset});
    vault->at(sfFlags) = txFlags & tfVaultPrivate;
    vault->at(sfSequence) = sequence;
    vault->at(sfOwner) = account_;
    vault->at(sfAccount) = pseudoId;
    vault->at(sfAssetsTotal) = Number(0);
    vault->at(sfAssetsAvailable) = Number(0);
    vault->at(sfLossUnrealized) = Number(0);
    // Leave default values for AssetTotal and AssetAvailable, both zero.
    if (auto value = tx[~sfAssetsMaximum])
        vault->at(sfAssetsMaximum) = *value;
    vault->at(sfShareMPTID) = mptIssuanceID;
    if (auto value = tx[~sfData])
        vault->at(sfData) = *value;
    // Required field, default to vaultStrategyFirstComeFirstServe
    if (auto value = tx[~sfWithdrawalPolicy])
        vault->at(sfWithdrawalPolicy) = *value;
    else
        vault->at(sfWithdrawalPolicy) = vaultStrategyFirstComeFirstServe;
    if (scale)
        vault->at(sfScale) = scale;
    if (auto value = tx[~sfVaultCode])
        vault->at(sfVaultCode) = *value;
    view().insert(vault);

    // Explicitly create MPToken for the vault owner
    if (auto const err = authorizeMPToken(
            view(), mPriorBalance, mptIssuanceID, account_, ctx_.journal);
        !isTesSuccess(err))
        return err;

    // If the vault is private, set the authorized flag for the vault owner
    if (txFlags & tfVaultPrivate)
    {
        if (auto const err = authorizeMPToken(
                view(),
                mPriorBalance,
                mptIssuanceID,
                pseudoId,
                ctx_.journal,
                {},
                account_);
            !isTesSuccess(err))
            return err;
    }

    return tesSUCCESS;
}

}  // namespace xrpl

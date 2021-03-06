#!/usr/bin/env python

from evilaccount import *

class DepositCharge(object):
	fee = 5.00
	def deposit_fee(self):
		self.withdraw(self.fee)
	
class WithdrawCharge(object):
	fee = 2.5
	def withdraw_fee(self):
		self.withdraw(self.fee)

class MostEvilAccount(EvilAccount, DepositCharge, WithdrawCharge):
	def deposit(self, amt):
		self.deposit_fee()
		super(MostEvilAccount, self).deposit(amt)
	def withdraw(self, amt):
		self.withdraw_fee()
		super(MostEvilAccount, self).withdraw(amt)


